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
    addSynth, addDrumSynth, addSampler, addAudioGenerator, removeGenerator,
    addReturn, removeReturn,
    addGeneratorEffect, addReturnEffect, addMasterEffect,
    removeEffect, moveEffect, toggleEffect,
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

void apply (Song& song, juce::UndoManager& um, const Step& step)
{
    const auto [rawOp, a, b] = step;
    const auto op = (Op) rawOp;

    um.beginNewTransaction();

    auto generators = song.getGenerators();
    auto returns = song.getReturns();

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

    switch (op)
    {
        case Op::addSynth:
            song.addGenerator ("Synth", Generator::synthType, &um);
            break;

        case Op::addDrumSynth:
            song.addGenerator ("Drums", Generator::drumSynthType, &um);
            break;

        case Op::addSampler:
            song.addGenerator ("Sampler", Generator::samplerType, &um);
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

        case Op::removeEffect:
        {
            // Whichever chain the first number lands in, counting the
            // generators, then the returns, then the master as one more.
            const auto chains = (int) generators.size() + (int) returns.size() + 1;
            const auto which = wrapIndex (a, chains);

            if (which < (int) generators.size())
            {
                auto owner = generators[(size_t) which];

                if (const auto effects = owner.getEffects(); ! effects.empty())
                    owner.removeEffect (effects[(size_t) wrapIndex (b, (int) effects.size())], &um);
            }
            else if (which < (int) generators.size() + (int) returns.size())
            {
                auto owner = returns[(size_t) (which - generators.size())];

                if (const auto effects = owner.getEffects(); ! effects.empty())
                    owner.removeEffect (effects[(size_t) wrapIndex (b, (int) effects.size())], &um);
            }
            else
            {
                auto owner = song.getMasterBus();

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
            apply (song, um, step);
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
            apply (song, um, step);

        sync::syncSongToEdit (song, *edit);

        while (um.undo())
        {
            sync::syncSongToEdit (song, *edit);
            RC_ASSERT (sync::findSyncProblems (song, *edit).empty());
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

        auto edit = te::Edit::createSingleTrackEdit (testEngine());
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
