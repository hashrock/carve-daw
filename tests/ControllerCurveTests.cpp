// A pattern's controller curves (the pitch wheel and the mod wheel): what a
// curve says between and around its points, and what a stroke of the pen and
// the eraser do to it.

#include <algorithm>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "app/NoteLanes.h"
#include "model/SongModel.h"

using namespace carve::model;
using Catch::Matchers::WithinAbs;

namespace
{
using Controller = Pattern::Controller;

struct Fixture
{
    Song song = Song::create ("Curves");
    juce::UndoManager um;
    Generator generator = song.addGenerator ("Lead", Generator::synthType, &um);
    Pattern pattern = generator.createPattern (&um, 4.0);
};
} // namespace

TEST_CASE ("A curve rests until its first point, then follows its points", "[song][controllers]")
{
    Fixture f;

    CHECK (! f.pattern.hasControllerPoints (Controller::pitchBend));
    CHECK (f.pattern.getControllerValue (Controller::pitchBend, 2.0) == 0.0f);

    f.pattern.replaceControllerPoints (Controller::pitchBend, 1.0, 2.0, { { 1.0, -1.0f }, { 2.0, 0.5f } }, &f.um);

    CHECK (f.pattern.getControllerValue (Controller::pitchBend, 0.5) == 0.0f);            // before: at rest
    CHECK (f.pattern.getControllerValue (Controller::pitchBend, 1.0) == -1.0f);
    CHECK_THAT (f.pattern.getControllerValue (Controller::pitchBend, 1.5), WithinAbs (-0.25, 1.0e-6));
    CHECK (f.pattern.getControllerValue (Controller::pitchBend, 3.5) == 0.5f);            // after: held

    // The other wheel is untouched, and the notes do not count the points.
    CHECK (! f.pattern.hasControllerPoints (Controller::modWheel));
    CHECK (f.pattern.getNumNotes() == 0);
}

TEST_CASE ("A stroke replaces what the curve had under it", "[song][controllers]")
{
    Fixture f;
    f.pattern.replaceControllerPoints (Controller::modWheel, 0.0, 4.0,
                                       { { 0.0, 0.1f }, { 1.0, 0.2f }, { 2.0, 0.3f }, { 3.0, 0.4f } }, &f.um);

    f.pattern.replaceControllerPoints (Controller::modWheel, 2.5, 0.5, { { 0.5, 0.9f }, { 2.5, 0.9f } }, &f.um);

    const auto points = f.pattern.getControllerPoints (Controller::modWheel);
    REQUIRE (points.size() == 4);
    CHECK (points[0].beat == 0.0);
    CHECK (points[1].beat == 0.5);
    CHECK (points[2].beat == 2.5);
    CHECK (points[3].beat == 3.0);

    // Values are clamped to the wheel's range, and beats to the pattern.
    f.pattern.replaceControllerPoints (Controller::modWheel, 3.5, 9.0, { { 3.5, 2.0f }, { 9.0, 0.5f } }, &f.um);
    CHECK (f.pattern.getControllerPoints (Controller::modWheel).back().value == 1.0f);
    CHECK (f.pattern.getControllerPoints (Controller::modWheel).back().beat == 3.5);
}

TEST_CASE ("The eraser takes the points it passes over", "[song][controllers]")
{
    Fixture f;
    f.pattern.replaceControllerPoints (Controller::pitchBend, 0.0, 3.0, { { 0.0, 0.5f }, { 1.5, -0.5f }, { 3.0, 0.0f } }, &f.um);

    f.pattern.removeControllerPoints (Controller::pitchBend, 2.0, 1.0, &f.um);
    CHECK (f.pattern.getControllerPoints (Controller::pitchBend).size() == 2);

    f.pattern.removeControllerPoints (Controller::pitchBend, 0.0, 4.0, &f.um);
    CHECK (! f.pattern.hasControllerPoints (Controller::pitchBend));
}

TEST_CASE ("A cloned pattern keeps its curves", "[song][controllers]")
{
    Fixture f;
    f.pattern.replaceControllerPoints (Controller::pitchBend, 1.0, 1.0, { { 1.0, 0.25f } }, &f.um);

    auto clone = f.generator.duplicatePattern (f.pattern, &f.um);
    CHECK (clone.getControllerPoints (Controller::pitchBend).size() == 1);
    CHECK (clone.getControllerValue (Controller::pitchBend, 2.0) == 0.25f);
}

TEST_CASE ("Only the instruments that read a wheel are said to", "[song][controllers]")
{
    Fixture f;
    const auto bend = Controller::pitchBend, mod = Controller::modWheel;

    CHECK (f.generator.readsController (bend));
    CHECK (! f.generator.readsController (mod));

    const auto font = f.song.addGenerator ("F", Generator::soundFontType, &f.um);
    CHECK (font.readsController (bend));
    CHECK (! font.readsController (mod));

    const auto plugin = f.song.addGenerator ("P", "plugin", &f.um);
    CHECK (plugin.readsController (bend));
    CHECK (plugin.readsController (mod));

    for (const auto* type : { Generator::drumKitType, Generator::vocalModelType, Generator::pianoType })
    {
        const auto other = f.song.addGenerator ("X", type, &f.um);
        CHECK (! other.readsController (bend));
        CHECK (! other.readsController (mod));
    }
}

//==============================================================================
// The lane under the roll reads and writes each per-note value as whole steps.

TEST_CASE ("Every lane step writes a value that reads back as the same step", "[song][controllers][lanes]")
{
    using carve::app::lanes::Kind;
    namespace lanes = carve::app::lanes;

    Fixture f;
    auto note = f.pattern.addNote (0.0, 1.0, 60, 100, &f.um);

    for (auto kind : { Kind::velocity, Kind::tune, Kind::retrigger, Kind::buzzPitch, Kind::slideOut })
    {
        for (int step = lanes::minOf (kind); step <= lanes::maxOf (kind); ++step)
        {
            lanes::setValue (kind, note, step, &f.um);
            INFO (lanes::nameOf (kind) << " step " << step);
            CHECK (lanes::valueOf (kind, note) == step);
        }

        // Past either end is the end.
        lanes::setValue (kind, note, lanes::maxOf (kind) + 5, &f.um);
        CHECK (lanes::valueOf (kind, note) == lanes::maxOf (kind));
        lanes::setValue (kind, note, lanes::minOf (kind) - 5, &f.um);
        CHECK (lanes::valueOf (kind, note) == lanes::minOf (kind));
    }

    // Retrigger step 0 is no roll at all, slide out step 0 no slide.
    lanes::setValue (Kind::retrigger, note, 0, &f.um);
    CHECK (note.getRetrigger() == 0);
    lanes::setValue (Kind::slideOut, note, -2, &f.um);
    CHECK (note.getSlideOut() == -Note::slideLengths[1]);
}

TEST_CASE ("A generator's lanes are the values its instrument plays", "[song][controllers][lanes]")
{
    using carve::app::lanes::Kind;
    namespace lanes = carve::app::lanes;

    Fixture f;
    auto has = [] (const std::vector<Kind>& kinds, Kind kind) { return std::find (kinds.begin(), kinds.end(), kind) != kinds.end(); };

    const auto synth = lanes::kindsFor (f.generator);
    CHECK (synth.front() == Kind::velocity);
    CHECK (has (synth, Kind::pitchBend));
    CHECK (! has (synth, Kind::modWheel));
    CHECK (! has (synth, Kind::tune));

    const auto slicer = lanes::kindsFor (f.song.addGenerator ("B", Generator::breakType, &f.um));
    CHECK (has (slicer, Kind::tune));
    CHECK (has (slicer, Kind::retrigger));
    CHECK (! has (slicer, Kind::pitchBend));

    const auto bass = lanes::kindsFor (f.song.addGenerator ("E", Generator::electricBassType, &f.um));
    CHECK (has (bass, Kind::slideOut));
}
