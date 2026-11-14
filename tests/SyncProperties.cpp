// Properties of the Edit the sync builds from a song.
//
// SongStateProperties.cpp drives the document itself: a generated sequence of
// edits, invariants after every one. That covers the model, and the model is
// only half of what the app is -- the other half is the tracktion Edit the
// model is projected onto, and every bug this file exists because of lived
// there:
//
//   * saving walked the returns and the generators and not the master, so a
//     limiter on the master came back at its defaults
//   * the mixer's staleness check knew generators and the master and not
//     returns, so a return bus's effect editor closed the moment it opened
//
// Group busses are a fourth kind of owner, and the routing that goes with
// them -- a generator in a group feeds the group's track, one in none feeds
// the master -- is checked here too.
//
// Both were the same shape: three kinds of owner, enumerated by hand in one
// place and enumerated differently in another. So the sequence here is made of
// the edits that add and remove owners and the things they own, and after
// every one the Edit is rebuilt and asked whether it still agrees with the
// song (sync::findSyncProblems, which a debug build asserts on too).
//
// This binary links the engine, which the model-layer tests deliberately do
// not: it is built in the app's own build directory so that tracktion is
// compiled once for both.
//
// The properties run 100 cases each by default; RC_PARAMS tunes that, as in
// the other files.

#include <optional>
#include <vector>

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include "EngineSetup.h"
#include "model/SongModel.h"
#include "sync/EditSync.h"
#include "sync/EffectChains.h"
#include "sync/EngineIds.h"

namespace
{

using namespace carve::model;
namespace sync = carve::sync;

//==============================================================================
// One engine for the whole run: building one opens the device manager and
// reads the settings folder, which is far too much to do per generated case.
// main() owns it, and the note down there says why it is not a static.
te::Engine* liveEngine = nullptr;

te::Engine& testEngine()
{
    return *liveEngine;
}

//==============================================================================
// The edits that move an owner or a chain around -- the surface the sync has
// to keep up with. Notes and their timing are SongStateProperties' business;
// nothing here is about them.
enum class Op
{
    addSynth, addDrumSynth, addGabberKick, addAcousticDrums, addElectricBass, addPiano, addBassModel, addSampler, addSoundFont, addUtau, addVocalModel, addBreak, addAudioGenerator, removeGenerator,
    addReturn, removeReturn,
    addGroup, removeGroup, setGroup,
    addGeneratorEffect, addReturnEffect, addGroupEffect, addMasterEffect, addMasteringEffect,
    removeEffect, moveEffect, toggleEffect,
    // The master's list holds two chains, so moving within either must leave
    // the other where it is -- and mastering mode bypasses one of them.
    moveMasterEffect, moveMasteringEffect, toggleMasteringEffect, setMasteringActive,
    addPattern, addClip,
    setSend,
    numOps
};

// An op, plus the two numbers that pick which of the things it acts on. Any
// value is legal: an op with nothing to act on is a step that does nothing,
// which is exactly the sequence a user produces by clicking Delete on an empty
// list.
using Step = std::tuple<int, int, int>;
using Steps = std::vector<Step>;

rc::Gen<Step> genStep()
{
    return rc::gen::tuple (rc::gen::inRange (0, (int) Op::numOps),
                           rc::gen::inRange (0, 32),
                           rc::gen::inRange (0, 32));
}

rc::Gen<Steps> genSteps()
{
    return rc::gen::container<Steps> (genStep());
}

int wrapIndex (int index, int size)
{
    return size <= 0 ? 0 : ((index % size) + size) % size;
}

// The effect types a step can insert. Internal ones only: an external plugin
// has no live counterpart on a machine that has never scanned it, which is a
// legitimate state rather than one the invariants should trip over.
const char* effectTypeFor (int index)
{
    static const char* types[] { "carveDelay", "carveLimiter", "carveOvertop",
                                 "reverb", "chorus", "4bandEq" };
    return types[(size_t) wrapIndex (index, 6)];
}

void apply (Song& song, juce::UndoManager& um, te::Edit& edit, const Step& step)
{
    const auto [rawOp, a, b] = step;
    const auto op = (Op) rawOp;

    um.beginNewTransaction();

    auto generators = song.getGenerators();
    auto returns = song.getReturns();
    auto groups = song.getGroups();

    auto pickGenerator = [&] () -> std::optional<Generator>
    {
        if (generators.empty())
            return std::nullopt;

        return generators[(size_t) wrapIndex (a, (int) generators.size())];
    };

    auto pickReturn = [&] () -> std::optional<Return>
    {
        if (returns.empty())
            return std::nullopt;

        return returns[(size_t) wrapIndex (a, (int) returns.size())];
    };

    auto pickGroup = [&] () -> std::optional<Group>
    {
        if (groups.empty())
            return std::nullopt;

        return groups[(size_t) wrapIndex (b, (int) groups.size())];
    };

    switch (op)
    {
        case Op::addSynth:
            song.addGenerator ("Synth", Generator::synthType, &um);
            break;

        case Op::addDrumSynth:
            song.addGenerator ("Drums", Generator::drumSynthType, &um);
            break;

        case Op::addGabberKick:
            song.addGenerator ("Kick", Generator::gabberKickType, &um);
            break;

        case Op::addAcousticDrums:
            song.addGenerator ("Kit", Generator::acousticDrumsType, &um);
            break;

        case Op::addElectricBass:
            song.addGenerator ("Bass", Generator::electricBassType, &um);
            break;

        case Op::addPiano:
            song.addGenerator ("Piano", Generator::pianoType, &um);
            break;

        case Op::addBassModel:
            song.addGenerator ("Bass Model", Generator::bassModelType, &um);
            break;

        case Op::addSampler:
            song.addGenerator ("Sampler", Generator::samplerType, &um);
            break;

        case Op::addSoundFont:
            // No file: an empty player is the case every new one passes
            // through, and the one that has to sync without reading anything.
            song.addGenerator ("SoundFont", Generator::soundFontType, &um);
            break;

        case Op::addUtau:
            // No voicebank, like the SoundFont case: what every new singer
            // syncs as before a folder is chosen.
            song.addGenerator ("Singer", Generator::utauType, &um);
            break;

        case Op::addVocalModel:
            // Its own state, like the internal instruments, and triggers in
            // its clips, like the singer above.
            song.addGenerator ("Vocal", Generator::vocalModelType, &um);
            break;

        case Op::addBreak:
            // No loop, like the two above.
            song.addGenerator ("Break", Generator::breakType, &um);
            break;

        case Op::addAudioGenerator:
            song.addGenerator ("Audio", Generator::audioType, &um);
            break;

        case Op::removeGenerator:
            if (auto generator = pickGenerator())
                song.removeGenerator (*generator, &um);
            break;

        case Op::addReturn:
            song.addReturn ("Bus", &um);
            break;

        case Op::removeReturn:
            if (auto bus = pickReturn())
                song.removeReturn (*bus, &um);
            break;

        case Op::addGroup:
            song.addGroup ("Group", &um);
            break;

        case Op::removeGroup:
            if (auto group = pickGroup())
                song.removeGroup (*group, &um);
            break;

        case Op::setGroup:
            // Into a group, or out of whatever it is in: both are the same
            // edit, and a generator dropped out of a group has to leave the
            // Edit's routing behind it.
            if (auto generator = pickGenerator())
            {
                if (auto group = pickGroup(); group && (a % 3) != 0)
                    generator->setGroupId (group->getId(), &um);
                else
                    generator->setGroupId ({}, &um);
            }
            break;

        case Op::addGroupEffect:
            if (auto group = pickGroup())
                group->addEffect (effectTypeFor (b), nullptr, &um);
            break;

        case Op::addGeneratorEffect:
            if (auto generator = pickGenerator())
                generator->addEffect (effectTypeFor (b), nullptr, &um);
            break;

        case Op::addReturnEffect:
            if (auto bus = pickReturn())
                bus->addEffect (effectTypeFor (b), nullptr, &um);
            break;

        case Op::addMasterEffect:
            song.getMasterBus().addEffect (effectTypeFor (b), nullptr, &um);
            break;

        case Op::addMasteringEffect:
            song.getMasteringChain().addEffect (effectTypeFor (b), nullptr, &um);
            break;

        case Op::moveMasterEffect:
        {
            auto owner = song.getMasterBus();

            if (const auto effects = owner.getEffects(); ! effects.empty())
                owner.moveEffect (effects[(size_t) wrapIndex (b, (int) effects.size())],
                                  wrapIndex (a, (int) effects.size()), &um);
            break;
        }

        case Op::moveMasteringEffect:
        {
            auto owner = song.getMasteringChain();

            if (const auto effects = owner.getEffects(); ! effects.empty())
                owner.moveEffect (effects[(size_t) wrapIndex (b, (int) effects.size())],
                                  wrapIndex (a, (int) effects.size()), &um);
            break;
        }

        case Op::toggleMasteringEffect:
            if (const auto effects = song.getMasteringChain().getEffects(); ! effects.empty())
            {
                auto effect = effects[(size_t) wrapIndex (b, (int) effects.size())];
                effect.setEnabled (! effect.isEnabled(), &um);
            }
            break;

        case Op::setMasteringActive:
            // Not a song edit -- nothing to undo -- but the next sync has to
            // honour it all the same.
            sync::setMasteringActive (edit, (a % 2) == 0);
            break;

        case Op::removeEffect:
        {
            // Whichever chain the first number lands in, counting the
            // generators, then the groups, then the returns, then the master
            // and the mastering chain as two more.
            const auto chains = (int) generators.size() + (int) groups.size()
                                 + (int) returns.size() + 2;
            const auto which = wrapIndex (a, chains);

            if (which < (int) generators.size())
            {
                auto owner = generators[(size_t) which];

                if (const auto effects = owner.getEffects(); ! effects.empty())
                    owner.removeEffect (effects[(size_t) wrapIndex (b, (int) effects.size())], &um);
            }
            else if (which < (int) (generators.size() + groups.size()))
            {
                auto owner = groups[(size_t) (which - (int) generators.size())];

                if (const auto effects = owner.getEffects(); ! effects.empty())
                    owner.removeEffect (effects[(size_t) wrapIndex (b, (int) effects.size())], &um);
            }
            else if (which < (int) (generators.size() + groups.size() + returns.size()))
            {
                auto owner = returns[(size_t) (which - (int) generators.size() - (int) groups.size())];

                if (const auto effects = owner.getEffects(); ! effects.empty())
                    owner.removeEffect (effects[(size_t) wrapIndex (b, (int) effects.size())], &um);
            }
            else if (which == chains - 2)
            {
                auto owner = song.getMasterBus();

                if (const auto effects = owner.getEffects(); ! effects.empty())
                    owner.removeEffect (effects[(size_t) wrapIndex (b, (int) effects.size())], &um);
            }
            else
            {
                auto owner = song.getMasteringChain();

                if (const auto effects = owner.getEffects(); ! effects.empty())
                    owner.removeEffect (effects[(size_t) wrapIndex (b, (int) effects.size())], &um);
            }

            break;
        }

        case Op::moveEffect:
            if (auto generator = pickGenerator())
                if (const auto effects = generator->getEffects(); ! effects.empty())
                    generator->moveEffect (effects[(size_t) wrapIndex (b, (int) effects.size())],
                                           wrapIndex (a, (int) effects.size()), &um);
            break;

        case Op::toggleEffect:
            if (auto generator = pickGenerator())
                if (const auto effects = generator->getEffects(); ! effects.empty())
                {
                    auto effect = effects[(size_t) wrapIndex (b, (int) effects.size())];
                    effect.setEnabled (! effect.isEnabled(), &um);
                }
            break;

        case Op::addPattern:
            if (auto generator = pickGenerator())
                generator->createPattern (&um, 4.0);
            break;

        case Op::addClip:
            if (auto generator = pickGenerator())
                if (const auto patterns = generator->getPatterns(); ! patterns.empty())
                    song.getPlaylist().addClip (*generator,
                                                patterns[(size_t) wrapIndex (b, (int) patterns.size())],
                                                (double) wrapIndex (b, 8), &um);
            break;

        case Op::setSend:
            if (auto generator = pickGenerator())
                if (auto bus = pickReturn())
                    generator->setSendGain (bus->getId(), -6.0f, &um);
            break;

        case Op::numOps:
            break;
    }
}

//==============================================================================
juce::String describe (const std::vector<juce::String>& problems)
{
    juce::String text;

    for (const auto& problem : problems)
        text += "\n  " + problem;

    return text;
}

} // namespace

//==============================================================================
TEST_CASE ("The edit the sync builds agrees with the song that built it", "[sync]")
{
    REQUIRE (rc::check ("no chain is left behind, whoever owns it", [] {
        auto song = Song::create ("Properties");
        juce::UndoManager um (1000 * 1000 * 1000, 1);
        auto edit = te::Edit::createSingleTrackEdit (testEngine());

        for (const auto& step : *genSteps())
        {
            apply (song, um, *edit, step);
            sync::syncSongToEdit (song, *edit);

            const auto problems = sync::findSyncProblems (song, *edit);

            if (! problems.empty())
                RC_LOG() << describe (problems).toStdString();

            RC_ASSERT (problems.empty());
        }
    }));
}

TEST_CASE ("Undoing a sequence leaves an edit that still agrees", "[sync][undo]")
{
    REQUIRE (rc::check ("walking back is as valid as walking forward", [] {
        auto song = Song::create ("Properties");
        juce::UndoManager um (1000 * 1000 * 1000, 1);
        auto edit = te::Edit::createSingleTrackEdit (testEngine());

        for (const auto& step : *genSteps())
            apply (song, um, *edit, step);

        sync::syncSongToEdit (song, *edit);

        while (um.undo())
        {
            sync::syncSongToEdit (song, *edit);

            const auto problems = sync::findSyncProblems (song, *edit);

            if (! problems.empty())
                RC_LOG() << describe (problems).toStdString();

            RC_ASSERT (problems.empty());
        }
    }));
}

//==============================================================================
// The other direction: what the live plugins hold has to come back into the
// song, or a knob moved since the last load is not in the file. This is the
// property the master chain failed for as long as it existed.
TEST_CASE ("What the plugins hold comes back into the song", "[sync][capture]")
{
    REQUIRE (rc::check ("every chain's settings survive a capture", [] {
        auto song = Song::create ("Capture");
        juce::UndoManager um (1000 * 1000 * 1000, 1);

        // One of every kind of owner, each with a chain on it.
        const auto generatorCount = *rc::gen::inRange (1, 4);
        const auto returnCount = *rc::gen::inRange (1, 3);
        const auto effectsPerChain = *rc::gen::inRange (1, 4);

        for (int i = 0; i < generatorCount; ++i)
            song.addGenerator ("G" + juce::String (i), Generator::synthType, &um);

        for (int i = 0; i < returnCount; ++i)
            song.addReturn ("R" + juce::String (i), &um);

        for (auto generator : song.getGenerators())
            for (int i = 0; i < effectsPerChain; ++i)
                generator.addEffect (effectTypeFor (i), nullptr, &um);

        for (auto bus : song.getReturns())
            for (int i = 0; i < effectsPerChain; ++i)
                bus.addEffect (effectTypeFor (i), nullptr, &um);

        for (int i = 0; i < effectsPerChain; ++i)
            song.getMasterBus().addEffect (effectTypeFor (i), nullptr, &um);

        for (int i = 0; i < effectsPerChain; ++i)
            song.getMasteringChain().addEffect (effectTypeFor (i), nullptr, &um);

        auto edit = te::Edit::createSingleTrackEdit (testEngine());

        // Bypassed or not, a mastering effect's settings are the song's.
        sync::setMasteringActive (*edit, *rc::gen::arbitrary<bool>());
        sync::syncSongToEdit (song, *edit);
        RC_ASSERT (sync::findSyncProblems (song, *edit).empty());

        // A mark in every live plugin, the way a knob move leaves one.
        static const juce::Identifier marker ("carveTestMark");
        int mark = 0;

        for (auto& site : sync::getEffectChainSites (song, *edit))
            for (const auto& effect : site.effects)
                if (auto plugin = site.findPlugin (effect.getId()))
                    plugin->state.setProperty (marker, ++mark, nullptr);

        RC_ASSERT (mark > 0);

        sync::captureLivePluginState (song, *edit);

        // ...and every one of them has to be in the song afterwards.
        int found = 0;

        for (auto& site : sync::getEffectChainSites (song, *edit))
        {
            for (const auto& effect : site.effects)
            {
                const auto stored = effect.getInternalState();
                RC_ASSERT (stored.isValid());
                RC_ASSERT (stored.hasProperty (marker));
                ++found;
            }
        }

        RC_ASSERT (found == mark);
    }));
}

//==============================================================================
// A generator's instrument is a live plugin whose knobs reach the song only
// when it is captured (on save). So the track that carries it has to stay
// that generator's however the others around it come and go: handed to the
// generator after it, it plays that one's notes with this one's sound, and
// the next save writes this one's knobs into the other.
namespace
{
const juce::Identifier ownerMark ("carveTestOwner");

juce::Array<te::AudioTrack*> generatorTracks (te::Edit& edit)
{
    juce::Array<te::AudioTrack*> tracks;

    for (auto track : te::getAudioTracks (edit))
        if (! sync::isBusTrack (*track))
            tracks.add (track);

    return tracks;
}

// Marks every live instrument with the generator it belongs to, the way a
// knob move leaves a mark only the live plugin knows about.
void markInstruments (const Song& song, te::Edit& edit)
{
    const auto generators = song.getGenerators();
    const auto tracks = generatorTracks (edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
        if (auto instrument = sync::findInstrumentPlugin (*tracks[i]))
            if (! instrument->state.hasProperty (ownerMark))
                instrument->state.setProperty (ownerMark, generators[(size_t) i].getId(), nullptr);
}

// Which generators' tracks are carrying some other generator's instrument.
std::vector<juce::String> strayInstruments (const Song& song, te::Edit& edit)
{
    std::vector<juce::String> stray;
    const auto generators = song.getGenerators();
    const auto tracks = generatorTracks (edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
        if (auto instrument = sync::findInstrumentPlugin (*tracks[i]))
            if (const auto owner = instrument->state[ownerMark].toString();
                owner.isNotEmpty() && owner != generators[(size_t) i].getId())
                stray.push_back (generators[(size_t) i].getName() + " plays " + owner + "'s instrument");

    return stray;
}
} // namespace

TEST_CASE ("A generator keeps its live instrument while others come and go", "[sync][tracks]")
{
    REQUIRE (rc::check ("no track is handed to another generator", [] {
        auto song = Song::create ("Tracks");
        juce::UndoManager um (1000 * 1000 * 1000, 1);
        auto edit = te::Edit::createSingleTrackEdit (testEngine());

        const auto count = *rc::gen::inRange (2, 6);

        for (int i = 0; i < count; ++i)
            song.addGenerator ("G" + juce::String (i), Generator::synthType, &um);

        sync::syncSongToEdit (song, *edit);
        markInstruments (song, *edit);

        // Removals and duplicates anywhere in the list, then all of it undone.
        const auto edits = *rc::gen::container<std::vector<std::pair<bool, int>>> (
            rc::gen::pair (rc::gen::arbitrary<bool>(), rc::gen::inRange (0, 100)));

        for (const auto& [remove, at] : edits)
        {
            const auto generators = song.getGenerators();

            if (generators.empty())
                break;

            const auto& target = generators[(size_t) (at % (int) generators.size())];
            um.beginNewTransaction();

            if (remove)
                song.removeGenerator (target, &um);
            else
                song.duplicateGenerator (target, &um);

            sync::syncSongToEdit (song, *edit);
            markInstruments (song, *edit);

            const auto stray = strayInstruments (song, *edit);
            RC_ASSERT (stray.empty());
            RC_ASSERT (sync::findSyncProblems (song, *edit).empty());
        }

        while (um.undo())
        {
            sync::syncSongToEdit (song, *edit);

            const auto stray = strayInstruments (song, *edit);
            RC_ASSERT (stray.empty());
            RC_ASSERT (sync::findSyncProblems (song, *edit).empty());
        }
    }));
}

//==============================================================================
// A pattern's pitch-wheel curve reaches its clips as controller events, back
// at rest by the end, and only on an instrument that reads the wheel.
TEST_CASE ("A drawn bend is played as the pitch wheel", "[sync][controllers]")
{
    auto song = Song::create ("Bend");
    juce::UndoManager um;
    auto synth = song.addGenerator ("Lead", Generator::synthType, &um);
    auto kit = song.addGenerator ("Kit", Generator::drumKitType, &um);

    for (auto generator : { synth, kit })
    {
        auto pattern = generator.createPattern (&um, 4.0);
        pattern.addNote (0.0, 4.0, 60, 100, &um);
        pattern.replaceControllerPoints (Pattern::Controller::pitchBend, 1.0, 2.0,
                                         { { 1.0, 0.0f }, { 2.0, 1.0f } }, &um);
        song.getPlaylist().addClip (generator, pattern, 0.0, &um);
    }

    auto edit = te::Edit::createSingleTrackEdit (testEngine());
    sync::syncSongToEdit (song, *edit);

    const auto tracks = generatorTracks (*edit);
    REQUIRE (tracks.size() == 2);

    auto controllersOn = [] (te::AudioTrack& track)
    {
        std::vector<std::pair<double, int>> events;

        for (auto clip : track.getClips())
            if (auto midi = dynamic_cast<te::MidiClip*> (clip))
                for (auto* event : midi->getSequence().getControllerEvents())
                    if (event->getType() == te::MidiControllerEvent::pitchWheelType)
                        events.emplace_back (event->getBeatPosition().inBeats(), event->getControllerValue());

        std::sort (events.begin(), events.end());
        return events;
    };

    const auto bend = controllersOn (*tracks[0]);
    REQUIRE (bend.size() > 10);   // a glide, not a jump
    CHECK (bend.front().second == 8192);
    CHECK (std::any_of (bend.begin(), bend.end(), [] (const auto& e) { return e.second == 16383; }));
    CHECK (bend.back().second == 8192);
    CHECK (bend.back().first < 4.0);

    for (size_t i = 1; i < bend.size(); ++i)
        CHECK (bend[i].second != bend[i - 1].second);   // no event that changes nothing

    CHECK (controllersOn (*tracks[1]).empty());

    // A second sync finds the clip as it should be and leaves it alone.
    auto* clip = dynamic_cast<te::MidiClip*> (tracks[0]->getClips()[0]);
    REQUIRE (clip != nullptr);
    const auto* first = clip->getSequence().getControllerEvents()[0];
    sync::syncSongToEdit (song, *edit);
    CHECK (clip->getSequence().getControllerEvents()[0] == first);
}

//==============================================================================
// The engine and JUCE's own state are owned here rather than by a static
// inside testEngine(), and the difference is not tidiness.
//
// A function-local static is destroyed by atexit, after main has returned,
// among every other static in the program -- Catch2's included. Torn down
// there, a run where every property passed still ended in SIGABRT: the engine
// went down, then ~ScopedJuceInitialiser_GUI walked JUCE's list of
// shutdown-owned singletons and freed something that was no longer a heap
// block. The app never hit it because JUCEApplicationBase owns the same two
// things in a scope and closes them in order, which is what this does.
int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceState;

    auto engine = carve::createEngine (std::make_unique<carve::HeadlessUIBehaviour>());
    liveEngine = engine.get();

    const auto result = Catch::Session().run (argc, argv);

    // Explicit, and in this order: the engine first, then JUCE as juceState
    // goes out of scope -- both before anything static is touched.
    liveEngine = nullptr;
    engine.reset();

    return result;
}
