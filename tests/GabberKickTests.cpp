// The Gabber Kick's synthesis (src/plugins/GabberKickDsp). Offline and
// deterministic, so it is checked here rather than through the engine.

#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "plugins/GabberKickDsp.h"

using namespace carve::plugins::gabber;

namespace
{
    double peakOf (const std::vector<float>& samples)
    {
        double peak = 0.0;

        for (auto s : samples)
            peak = std::max (peak, (double) std::abs (s));

        return peak;
    }

    bool allFinite (const std::vector<float>& samples)
    {
        for (auto s : samples)
            if (! std::isfinite (s))
                return false;

        return true;
    }
}

TEST_CASE ("every factory preset renders audible finite audio at its ceiling", "[gabber]")
{
    for (int i = 0; i < numFactoryPresets; ++i)
    {
        const auto result = render (getFactoryPreset (i), 44100.0);

        CHECK (allFinite (result.samples));
        CHECK (std::abs (peakOf (result.samples) - std::pow (10.0, -1.0 / 20.0)) < 1.0e-3);
        CHECK_FALSE (result.limited);
    }
}

TEST_CASE ("the same patch renders the same samples", "[gabber]")
{
    const auto patch = getFactoryPreset (2);
    CHECK (render (patch, 48000.0).samples == render (patch, 48000.0).samples);
}

TEST_CASE ("the kick's length follows the output rate", "[gabber]")
{
    auto patch = getFactoryPreset (0);
    patch.duration = 0.5;

    CHECK (render (patch, 44100.0).samples.size() == 22050);
    CHECK (render (patch, 96000.0).samples.size() == 48000);
}

TEST_CASE ("a bypassed stage is the same as no stage", "[gabber]")
{
    auto bypassed = getFactoryPreset (0);
    auto removed = bypassed;

    bypassed.stages[1].enabled = false;
    removed.stages.erase (removed.stages.begin() + 1);

    CHECK (render (bypassed, 44100.0).samples == render (removed, 44100.0).samples);
}

TEST_CASE ("stage order changes the sound", "[gabber]")
{
    auto patch = getFactoryPreset (1);
    const auto before = render (patch, 44100.0).samples;

    std::swap (patch.stages[0], patch.stages[1]);
    CHECK (render (patch, 44100.0).samples != before);
}

TEST_CASE ("extreme settings stay finite and under the ceiling", "[gabber]")
{
    auto patch = getFactoryPreset (3);
    patch.gain = 12.0;

    for (auto& stage : patch.stages)
    {
        stage.startGain = 24.0;
        stage.endGain = -24.0;
        stage.startFreq = 20.0;
        stage.endFreq = 18000.0;
        stage.q = 10.0;
        stage.drive = 36.0;
        stage.output = 12.0;
    }

    const auto result = render (patch, 44100.0);

    CHECK (result.limited);
    CHECK (allFinite (result.samples));
    CHECK (peakOf (result.samples) <= std::pow (10.0, -0.1 / 20.0) + 1.0e-6);
}

TEST_CASE ("garbage in a patch is clamped, not rendered", "[gabber]")
{
    auto patch = getFactoryPreset (0);
    patch.base = std::nan ("");
    patch.duration = 1000.0;
    patch.stages[0].points.clear();
    patch.stages[0].drive = std::numeric_limits<double>::infinity();

    const auto result = render (patch, 44100.0);

    CHECK (result.samples.size() == 52920);   // 1.2 s, the longest a kick is
    CHECK (allFinite (result.samples));
}

TEST_CASE ("stages survive being stored as JSON", "[gabber]")
{
    const auto stages = getFactoryPreset (1).stages;
    const auto read = stagesFromJson (stagesToJson (stages));

    REQUIRE (read.size() == stages.size());

    for (size_t i = 0; i < stages.size(); ++i)
    {
        CHECK (read[i].shape == stages[i].shape);
        CHECK (read[i].mix == stages[i].mix);
        REQUIRE (read[i].points.size() == stages[i].points.size());
        CHECK (std::abs (read[i].points[3].y - stages[i].points[3].y) < 1.0e-9);
    }
}

TEST_CASE ("malformed stage JSON reads as nothing, and missing fields as defaults", "[gabber]")
{
    CHECK (stagesFromJson ("not json").empty());
    CHECK (stagesFromJson ("[1, 2]").empty());

    const auto read = stagesFromJson (R"([{"drive": 3, "shape": "Wavefold"}])");

    REQUIRE (read.size() == 1);
    CHECK (read[0].drive == 3.0);
    CHECK (read[0].shape == Shape::wavefold);
    CHECK (read[0].points == getShapePoints (Shape::wavefold));
    CHECK (read[0].q == 1.0);
}
