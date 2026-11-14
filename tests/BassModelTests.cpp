// The modelled bass's synthesis (src/plugins/BassModel). Deterministic and
// free of the engine, so it is checked here: that notes sound at their pitch
// on every string, hammered and slid to as well as plucked; that no playing
// style or setting blows up; that a note let go of dies away and one held
// rings on; and that every factory sound plays at a sensible level. The
// prototype (web-bass-model's tools/selftest.mjs) checks the same things
// more finely.

#include <algorithm>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "plugins/BassModel.h"

using namespace carve::plugins::bassmodel;

namespace
{
    constexpr double fs = 48000.0;

    std::vector<float> render (Bass& b, double seconds)
    {
        std::vector<float> out ((size_t) (seconds * fs), 0.0f);
        b.process (out.data(), (int) out.size());
        return out;
    }

    double rms (const std::vector<float>& x, double from, double to)
    {
        double sum = 0.0;
        const auto i0 = (size_t) (from * fs), i1 = (size_t) (to * fs);

        for (size_t i = i0; i < i1; ++i)
            sum += (double) x[i] * x[i];

        return std::sqrt (sum / (double) (i1 - i0));
    }

    double db (double ratio)  { return 20.0 * std::log10 (ratio + 1e-30); }

    // The strongest frequency near `expected` in [from, to) seconds, by a fine
    // DFT scan. The partials are stretched by the strings' stiffness, so the
    // fundamental is what is measured.
    double measuredPitch (const std::vector<float>& x, double from, double to, double expected)
    {
        const auto i0 = (size_t) (from * fs), n = (size_t) ((to - from) * fs);
        double best = 0.0, bestPower = -1.0;

        for (double cents = -15.0; cents <= 15.0; cents += 0.25)
        {
            const double f = expected * std::pow (2.0, cents / 1200.0);
            double re = 0.0, im = 0.0;

            for (size_t i = 0; i < n; ++i)
            {
                const double w = 0.5 - 0.5 * std::cos (6.283185307179586 * (double) i / (double) n);
                const double ph = 6.283185307179586 * f * (double) i / fs;
                re += w * x[i0 + i] * std::cos (ph);
                im += w * x[i0 + i] * std::sin (ph);
            }

            if (re * re + im * im > bestPower)
            {
                bestPower = re * re + im * im;
                best = f;
            }
        }

        return best;
    }

    double centsOff (double measured, int midi)  { return 1200.0 * std::log2 (measured / midiToHz (midi)); }
}

TEST_CASE ("Bass model: every string sounds in tune up the neck", "[bassmodel]")
{
    for (int si = 0; si < numStrings; ++si)
        for (int fret = 0; fret <= numFrets; fret += 5)
        {
            Bass b;
            b.prepare (fs);
            b.lastFret = fret;
            const int midi = tuning[(size_t) si] + fret;

            if (b.place (midi).string != si)
                continue;   // another string is nearer the hand

            b.noteOn (midi, 0.7);
            const auto out = render (b, 1.2);
            INFO ("string " << si << " fret " << fret);
            CHECK (std::abs (centsOff (measuredPitch (out, 0.3, 1.1, midiToHz (midi)), midi)) < 2.0);
        }
}

TEST_CASE ("Bass model: no style or extreme setting blows up", "[bassmodel]")
{
    for (int style = 0; style <= 4; ++style)
        for (double action : { 0.0, 1.0 })
            for (double height : { 0.0, 1.0 })
            {
                Options o;
                o.style = style;
                o.action = action;
                o.height = height;
                o.drive = 1.0;
                o.volume = 1.5;

                Bass b;
                b.setOptions (o);
                b.prepare (fs);
                b.noteOn (28, 1.0);
                auto out = render (b, 0.3);
                b.noteOn (40, 1.0);
                b.noteOn (55, 1.0);
                const auto more = render (b, 0.5);
                out.insert (out.end(), more.begin(), more.end());

                bool finite = true;
                float peak = 0.0f;

                for (float x : out)
                {
                    finite = finite && std::isfinite (x);
                    peak = std::max (peak, std::abs (x));
                }

                INFO ("style " << style << " action " << action << " height " << height);
                CHECK (finite);
                CHECK (peak <= 1.0f);
                CHECK (peak > 0.01f);
            }
}

TEST_CASE ("Bass model: hammer-ons and slides land in tune without a new pluck", "[bassmodel]")
{
    for (int mode = 0; mode < 3; ++mode)
    {
        Options o;
        o.fretless = mode == 2;

        Bass b;
        b.setOptions (o);
        b.prepare (fs);
        b.noteOn (33, 0.8);
        render (b, 0.45);

        const int target = mode == 0 ? 35 : 38;
        b.noteOn (target, 0.8, mode == 0, mode != 0);
        b.noteOff (33);
        const auto out = render (b, 1.5);

        INFO ((mode == 0 ? "hammer-on" : mode == 1 ? "fretted slide" : "fretless slide"));
        CHECK (std::abs (centsOff (measuredPitch (out, 0.3, 1.2, midiToHz (target)), target)) < 3.0);
        // The same string carried on: only one is sounding.
        CHECK (b.getSoundingCount() == 1);
    }
}

TEST_CASE ("Bass model: a note let go dies quickly, one held rings on", "[bassmodel]")
{
    Bass let, held;
    let.prepare (fs);
    held.prepare (fs);
    let.noteOn (33, 0.8);
    held.noteOn (33, 0.8);

    auto a = render (let, 0.55);
    let.noteOff (33);
    const auto rest = render (let, 0.8);
    a.insert (a.end(), rest.begin(), rest.end());
    const auto h = render (held, 3.0);

    CHECK (db (rms (a, 0.85, 0.95) / rms (a, 0.05, 0.45)) < -40.0);
    CHECK (db (rms (h, 2.5, 2.9) / rms (h, 0.05, 0.45)) > -30.0);
}

TEST_CASE ("Bass model: the same notes render the same", "[bassmodel]")
{
    auto once = []
    {
        Bass b;
        b.prepare (fs, 7);
        b.noteOn (28, 0.9);
        auto out = render (b, 0.2);
        b.noteOn (31, 0.8);
        const auto more = render (b, 0.3);
        out.insert (out.end(), more.begin(), more.end());
        return out;
    };

    CHECK (once() == once());
}

TEST_CASE ("Bass model: every factory sound plays at a sensible level", "[bassmodel]")
{
    for (int i = 0; i < numFactoryPresets; ++i)
    {
        Bass b;
        b.setOptions (getFactoryPreset (i));
        b.prepare (fs);

        std::vector<float> out;

        for (int midi : { 28, 33, 40, 36 })
        {
            b.noteOn (midi, 0.85);
            const auto part = render (b, 0.4);
            out.insert (out.end(), part.begin(), part.end());
            b.noteOff (midi);
        }

        float peak = 0.0f;

        for (float x : out)
            peak = std::max (peak, std::abs (x));

        INFO (getFactoryPresetName (i));
        const double level = db (rms (out, 0.0, (double) out.size() / fs));
        CHECK (level > -32.0);
        CHECK (level < -8.0);
        CHECK (peak < 1.0f);
    }
}
