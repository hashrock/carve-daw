// The modelled piano's synthesis (src/plugins/PianoModel). Deterministic and
// free of the engine, so it is checked here: that keys sound at their pitch,
// that nothing blows up, and that what is let go of dies away. The prototype
// (web-piano's tools/selftest.mjs) checks the same things more finely.

#include <algorithm>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "plugins/PianoModel.h"

using namespace carve::plugins::piano;

namespace
{
    constexpr double fs = 48000.0;

    struct Stereo
    {
        std::vector<float> l, r;

        explicit Stereo (int n) : l ((size_t) n, 0.0f), r ((size_t) n, 0.0f) {}

        bool finite() const
        {
            for (size_t i = 0; i < l.size(); ++i)
                if (! std::isfinite (l[i]) || ! std::isfinite (r[i]))
                    return false;

            return true;
        }

        double rms (int from, int to) const
        {
            double sum = 0.0;

            for (int i = from; i < to; ++i)
                sum += 0.5 * ((double) l[(size_t) i] * l[(size_t) i] + (double) r[(size_t) i] * r[(size_t) i]);

            return std::sqrt (sum / (to - from));
        }
    };

    void render (Piano& p, Stereo& out, int from, int to)
    {
        p.process (out.l.data() + from, out.r.data() + from, to - from);
    }

    // The strongest frequency near `expected`, by a fine DFT scan of the
    // sustained part of the note (the room and board kept out of it).
    double measuredPitch (const std::vector<float>& x, double expected)
    {
        double best = 0.0, bestPower = -1.0;

        for (double cents = -30.0; cents <= 30.0; cents += 0.1)
        {
            const double f = expected * std::pow (2.0, cents / 1200.0);
            double re = 0.0, im = 0.0;

            for (size_t i = 0; i < x.size(); ++i)
            {
                const double w = 0.5 - 0.5 * std::cos (6.283185307179586 * (double) i / (double) x.size());
                const double ph = 6.283185307179586 * f * (double) i / fs;
                re += w * x[i] * std::cos (ph);
                im += w * x[i] * std::sin (ph);
            }

            if (re * re + im * im > bestPower)
            {
                bestPower = re * re + im * im;
                best = f;
            }
        }

        return best;
    }
}

TEST_CASE ("piano keys sound at their stretched pitch", "[piano]")
{
    Options dry;
    dry.reverb = 0.0;
    dry.board = 0.0;
    dry.detune = 0.0;

    for (int key : { 33, 48, 60, 69, 84 })
    {
        Piano p;
        p.prepare (fs);
        p.setOptions (dry);
        p.noteOn (key, 0.6);

        Stereo out ((int) fs);
        render (p, out, 0, (int) fs);

        const std::vector<float> tail (out.l.begin() + (int) (0.3 * fs), out.l.end());
        const double s = (key - 69) / 39.0;
        const double expected = 440.0 * std::pow (2.0, (key - 69) / 12.0 + 20.0 * s * std::abs (s) / 1200.0);
        const double cents = 1200.0 * std::log2 (measuredPitch (tail, expected) / expected);

        INFO ("key " << key << " is off by " << cents << " cents");
        CHECK (std::abs (cents) < 1.5);
    }
}

TEST_CASE ("forty keys struck as hard as possible stay finite and bounded", "[piano]")
{
    Piano p;
    p.prepare (fs);

    for (int k = 0; k < 40; ++k)
        p.noteOn (lowestKey + k * 2, 1.0);

    Stereo out ((int) (2 * fs));
    render (p, out, 0, (int) (2 * fs));

    CHECK (out.finite());

    float peak = 0.0f;

    for (size_t i = 0; i < out.l.size(); ++i)
        peak = std::max ({ peak, std::abs (out.l[i]), std::abs (out.r[i]) });

    CHECK (peak > 0.1f);
    CHECK (peak <= 1.0f);
}

TEST_CASE ("a released key dies away and its voice is freed", "[piano]")
{
    Piano p;
    p.prepare (fs);
    p.noteOn (60, 0.8);

    const int n = (int) (8 * fs);
    Stereo out (n);
    render (p, out, 0, (int) (0.5 * fs));
    p.noteOff (60);
    render (p, out, (int) (0.5 * fs), n);

    CHECK (out.rms (0, (int) (0.5 * fs)) > 1e-3);
    CHECK (out.rms (n - (int) fs, n) < 1e-5);
    CHECK (p.getVoiceCount() == 0);
}

TEST_CASE ("the pedal keeps a released key ringing", "[piano]")
{
    auto ringAfterRelease = [] (bool pedal)
    {
        Piano p;
        p.prepare (fs);
        p.setPedal (pedal);
        p.noteOn (48, 0.8);

        Stereo out ((int) (2 * fs));
        render (p, out, 0, (int) (0.3 * fs));
        p.noteOff (48);
        render (p, out, (int) (0.3 * fs), (int) (2 * fs));
        return out.rms ((int) (1.5 * fs), (int) (2 * fs));
    };

    CHECK (ringAfterRelease (true) > 10.0 * ringAfterRelease (false));
}

TEST_CASE ("harder strikes are louder", "[piano]")
{
    auto level = [] (double velocity)
    {
        Piano p;
        p.prepare (fs);
        p.noteOn (60, velocity);
        Stereo out ((int) (0.5 * fs));
        render (p, out, 0, (int) (0.5 * fs));
        return out.rms (0, (int) (0.5 * fs));
    };

    CHECK (level (0.2) < level (0.5));
    CHECK (level (0.5) < level (0.9));
}

TEST_CASE ("keys outside A0..C8 are ignored", "[piano]")
{
    Piano p;
    p.prepare (fs);
    p.noteOn (20, 1.0);
    p.noteOn (109, 1.0);
    CHECK (p.getVoiceCount() == 0);
}

TEST_CASE ("once everything has rung down the piano sleeps, and a key wakes it", "[piano]")
{
    Piano p;
    p.prepare (fs);
    CHECK (p.isSilent());

    p.noteOn (60, 0.8);
    CHECK (! p.isSilent());
    p.noteOff (60);

    // The key's damped decay, then the room's tail.
    Stereo out ((int) (8 * fs));
    render (p, out, 0, (int) (8 * fs));
    CHECK (p.isSilent());

    // Asleep, it adds nothing.
    Stereo quiet (512);
    render (p, quiet, 0, 512);
    CHECK (quiet.rms (0, 512) == 0.0);

    p.noteOn (60, 0.8);
    Stereo again ((int) (0.2 * fs));
    render (p, again, 0, (int) (0.2 * fs));
    CHECK (again.rms (0, (int) (0.2 * fs)) > 1e-3);
}
