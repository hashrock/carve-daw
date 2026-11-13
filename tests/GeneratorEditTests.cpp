// Copying a generator and swapping its instrument: the two edits that take a
// whole generator apart and put it back together, so the ones where a
// reference left pointing at the wrong thing would go unnoticed until a
// knob or a clip did something odd.

#include <catch2/catch_test_macros.hpp>

#include "model/SongModel.h"

using namespace carve::model;

namespace
{
// A synth with a bit of everything that can point at something else.
struct Fixture
{
    Song song = Song::create ("Copies");
    juce::UndoManager um;
    Generator source = song.addGenerator ("Lead", Generator::synthType, &um);
    Pattern pattern = source.createPattern (&um, 4.0);
    Effect compressor = source.addEffect ("compressor", nullptr, &um);

    Fixture()
    {
        pattern.addNote (0.0, 1.0, 60, 100, &um);
        compressor.setSidechainSourceId (source.getId(), &um);

        source.addAutomationLane (compressor.getId(), "threshold", &um).addPoint (1.0, 0.5f, &um);
        source.addAutomationLane (AutomationLane::instrumentTarget, "cutoff", &um).addPoint (2.0, 0.3f, &um);

        auto lfo = source.addModifier (GenModifier::lfoKind, &um);
        lfo.addAssign (compressor.getId(), "ratio", &um);
        lfo.addAssign (AutomationLane::instrumentTarget, "cutoff", &um);

        juce::ValueTree knobs ("PLUGIN");
        knobs.setProperty ("type", "4osc", nullptr);
        source.setInternalState (knobs, &um);

        song.getPlaylist().addClip (source, pattern, 8.0, &um);
    }
};
} // namespace

TEST_CASE ("A copied generator owns copies of everything in it", "[song][generators]")
{
    Fixture f;
    auto other = f.song.addGenerator ("Bass", Generator::synthType, &f.um);
    auto copy = f.song.duplicateGenerator (f.source, &f.um);

    // Straight after its source, under its own name and id.
    REQUIRE (f.song.getGenerator (1).getId() == copy.getId());
    REQUIRE (f.song.getGenerator (2).getId() == other.getId());
    CHECK (copy.getName() == "Lead copy");
    CHECK (copy.getId() != f.source.getId());
    CHECK (f.song.duplicateGenerator (f.source, &f.um).getName() == "Lead copy 2");

    // Its pattern, notes and all, under a fresh id.
    REQUIRE (copy.getNumPatterns() == 1);
    const auto copiedPattern = copy.getPattern (0);
    CHECK (copiedPattern.getId() != f.pattern.getId());
    CHECK (copiedPattern.getName() == f.pattern.getName());
    CHECK (copiedPattern.getNumNotes() == 1);

    // The instrument's settings.
    CHECK (copy.getInternalState()["type"].toString() == "4osc");

    // Its effect, under a fresh id, keyed from the copy rather than the source.
    REQUIRE (copy.getEffects().size() == 1);
    const auto copiedEffect = copy.getEffects()[0];
    CHECK (copiedEffect.getId() != f.compressor.getId());
    CHECK (copiedEffect.getSidechainSourceId() == copy.getId());
    CHECK (f.compressor.getSidechainSourceId() == f.source.getId());

    // The lanes and the modulator follow the effect to its new id.
    CHECK (copy.findAutomationLane (copiedEffect.getId(), "threshold").has_value());
    CHECK (copy.findAutomationLane (AutomationLane::instrumentTarget, "cutoff").has_value());
    CHECK (! copy.findAutomationLane (f.compressor.getId(), "threshold").has_value());

    REQUIRE (copy.getModifiers().size() == 1);
    CHECK (copy.getModifiers()[0].getId() != f.source.getModifiers()[0].getId());
    CHECK (copy.getModifiers()[0].getAssigns()[0].getTarget() == copiedEffect.getId());

    // Its clip, on its own row, playing its own pattern.
    int sourceClips = 0, copyClips = 0;

    for (const auto& clip : f.song.getPlaylist().getClips())
    {
        if (clip.getGeneratorId() == f.source.getId())
            ++sourceClips;

        if (clip.getGeneratorId() == copy.getId())
        {
            ++copyClips;
            CHECK (clip.getPatternId() == copiedPattern.getId());
            CHECK (clip.getStart() == 8.0);
        }
    }

    CHECK (sourceClips == 1);
    CHECK (copyClips == 1);
}

TEST_CASE ("A copy is one undo step", "[song][generators][undo]")
{
    Fixture f;
    const auto before = f.song.state.toXmlString();

    f.um.beginNewTransaction();
    f.song.duplicateGenerator (f.source, &f.um);
    REQUIRE (f.song.getNumGenerators() == 2);

    f.um.undo();
    CHECK (f.song.state.toXmlString() == before);
}

TEST_CASE ("Replacing the instrument keeps the notes and what they play through", "[song][generators]")
{
    Fixture f;
    f.source.replaceInstrument (Generator::pianoType, nullptr, &f.um);

    CHECK (f.source.isPiano());
    CHECK (f.source.getName() == "Lead");

    // The old instrument's settings, and what was aimed at its parameters.
    CHECK (! f.source.getInternalState().isValid());
    CHECK (! f.source.findAutomationLane (AutomationLane::instrumentTarget, "cutoff").has_value());
    REQUIRE (f.source.getModifiers().size() == 1);
    REQUIRE (f.source.getModifiers()[0].getAssigns().size() == 1);
    CHECK (f.source.getModifiers()[0].getAssigns()[0].getTarget() == f.compressor.getId());

    // Everything else.
    CHECK (f.source.getNumPatterns() == 1);
    CHECK (f.source.getPattern (0).getNumNotes() == 1);
    CHECK (f.source.getEffects().size() == 1);
    CHECK (f.source.findAutomationLane (f.compressor.getId(), "threshold").has_value());
    CHECK (f.song.getPlaylist().getClips().size() == 1);
}

TEST_CASE ("Replacing a sampler's instrument lets go of its samples", "[song][generators]")
{
    Song song = Song::create ("Swap");
    juce::UndoManager um;
    auto sampler = song.addGenerator ("Hit", Generator::samplerType, &um);
    sampler.setSingleSound (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hit.wav"), &um);
    REQUIRE (sampler.getSounds().size() == 1);

    um.beginNewTransaction();
    sampler.replaceInstrument (Generator::synthType, nullptr, &um);
    CHECK (sampler.getSounds().empty());
    CHECK (sampler.getType() == Generator::synthType);

    um.undo();
    CHECK (sampler.isSampler());
    CHECK (sampler.getSounds().size() == 1);
}
