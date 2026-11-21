// The modelled kit (src/plugins/DrumModel*): that the models compiled in load
// and every piece of the kit sounds at a sensible level at the common host
// rates; that a drum rings down and falls asleep; that the hats choke; that
// the knobs at their extremes do not blow anything up; that the engines are
// deterministic; and that the resampler passes the band at unity. That the
// engines match the PhysKit originals (player/rt/drum.wasm and snare.wasm,
// rendered with the same hits) was checked when they were ported -- toms and
// kick to rounding, the snare to 0.04dB in octave-band energy (its wires are
// chaotic) -- and needs the PhysKit project, so it is not repeated here.

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "plugins/DrumModelKit.h"

using namespace carve::plugins::drummodel;

namespace
{
    struct Render
    {
        std::vector<float> l, r;

        double peakDb (double from, double to, double fs) const
        {
            float p = 0;
            for (auto i = (size_t) (from * fs); i < (size_t) (to * fs) && i < l.size(); ++i)
                p = std::max ({ p, std::abs (l[i]), std::abs (r[i]) });
            return 20 * std::log10 (p + 1e-30);
        }

        bool finite() const
        {
            for (size_t i = 0; i < l.size(); ++i)
                if (! std::isfinite (l[i]) || ! std::isfinite (r[i]))
                    return false;
            return true;
        }
    };

    Render render (Kit& kit, double fs, double seconds, int block = 256)
    {
        Render out;
        const auto n = (size_t) (seconds * fs);
        out.l.assign (n, 0.0f);
        out.r.assign (n, 0.0f);
        for (size_t i = 0; i < n; i += (size_t) block)
            kit.process (out.l.data() + i, out.r.data() + i, (int) std::min<size_t> ((size_t) block, n - i));
        return out;
    }

    Kit& loadedKit()
    {
        static Kit kit;
        static const bool ok = kit.load();
        REQUIRE (ok);
        return kit;
    }
}

TEST_CASE ("Modelled Drums: every piece sounds at a sensible level", "[drummodel]")
{
    auto& kit = loadedKit();
    REQUIRE (kit.isLoaded());

    const Piece pieces[] { Piece::kick, Piece::snare, Piece::rimshot, Piece::tomHigh, Piece::tomMid, Piece::tomFloor,
                           Piece::hatClosed, Piece::hatOpen, Piece::hatPedal, Piece::crash, Piece::crashChoke,
                           Piece::ride, Piece::rideBell };

    for (const double fs : { 48000.0, 44100.0 })
    {
        kit.prepare (fs, 512);
        Settings s;
        s.room = 0;   // the room's impulse loads in the background
        kit.setSettings (s);

        for (const auto p : pieces)
        {
            kit.allNotesOff();
            const auto silent = render (kit, fs, 0.05);
            CHECK (silent.peakDb (0, 0.05, fs) < -100);

            kit.hit (p, 0.8);
            const auto out = render (kit, fs, 0.4);
            INFO (pieceName (p) << " at " << fs);
            CHECK (out.finite());
            const double peak = out.peakDb (0, 0.4, fs);
            CHECK (peak > -30);
            CHECK (peak < 0.5);   // the limiter's -3dB, overshooting by a little at most
        }
    }

    // Softer is quieter.
    kit.prepare (48000, 512);
    for (const auto p : { Piece::kick, Piece::snare, Piece::tomMid })
    {
        kit.allNotesOff();
        kit.hit (p, 0.2);
        const double soft = render (kit, 48000, 0.3).peakDb (0, 0.3, 48000);
        kit.allNotesOff();
        kit.hit (p, 0.7);
        const double hard = render (kit, 48000, 0.3).peakDb (0, 0.3, 48000);
        INFO (pieceName (p));
        CHECK (hard > soft + 6);
    }
}

TEST_CASE ("Modelled Drums: a tom rings down and goes to sleep", "[drummodel]")
{
    auto& kit = loadedKit();
    kit.prepare (48000, 512);
    kit.allNotesOff();
    kit.hit (Piece::tomHigh, 0.7);
    const auto out = render (kit, 48000, 6.0);
    CHECK (out.peakDb (0, 0.2, 48000) > -20);
    CHECK (out.peakDb (5.0, 6.0, 48000) < -90);
    CHECK (kit.getActivity().toms[0] == 0);
}

TEST_CASE ("Modelled Drums: a closed hat chokes an open one", "[drummodel]")
{
    auto& kit = loadedKit();
    kit.prepare (48000, 512);
    kit.allNotesOff();

    kit.hit (Piece::hatOpen, 0.8);
    const auto ringing = render (kit, 48000, 1.2);
    kit.allNotesOff();

    kit.hit (Piece::hatOpen, 0.8);
    auto a = render (kit, 48000, 0.6);
    kit.hit (Piece::hatClosed, 0.2);
    auto b = render (kit, 48000, 0.6);

    // The open hat alone is still well up 1.0-1.2s in; choked, nothing is left
    // but the closed hat's own short sound (gone by then).
    CHECK (ringing.peakDb (1.0, 1.2, 48000) > -50);
    CHECK (b.peakDb (0.4, 0.6, 48000) < ringing.peakDb (1.0, 1.2, 48000) - 20);
}

TEST_CASE ("Modelled Drums: the knobs at their extremes stay finite", "[drummodel]")
{
    auto& kit = loadedKit();
    kit.prepare (48000, 512);

    for (int corner = 0; corner < 2; ++corner)
    {
        Settings s;
        const bool hi = corner == 1;
        for (auto* d : { &s.toms[0], &s.toms[1], &s.toms[2], &s.kick, &s.snare })
        {
            d->tune = hi ? 7 : -7;
            d->mute = hi ? 2 : -1;
            d->glide = hi ? 3 : 0;
            d->stick = hi ? 2 : -2;
        }
        s.kick.mute = hi ? 3.5 : 0;
        s.snareWires = hi ? 3 : 0;
        s.hihat.tune = s.crash.tune = s.ride.tune = hi ? 7 : -24;
        s.close = s.overhead = hi ? 2 : 0;
        s.room = 0;
        s.volume = 1.5;
        kit.setSettings (s);
        kit.allNotesOff();

        for (const auto p : { Piece::kick, Piece::snare, Piece::rimshot, Piece::tomHigh, Piece::tomMid, Piece::tomFloor, Piece::crash })
            kit.hit (p, 1.0);

        const auto out = render (kit, 48000, 1.0);
        INFO ("corner " << corner);
        CHECK (out.finite());
        CHECK (out.peakDb (0, 1.0, 48000) < 1.0);
    }

    kit.setSettings ({});
}

TEST_CASE ("Modelled Drums: the engines are deterministic", "[drummodel]")
{
    Kit a, b;
    REQUIRE (a.load());
    REQUIRE (b.load());

    for (auto* k : { &a, &b })
    {
        k->prepare (44100, 256);
        Settings s;
        s.room = 0;
        k->setSettings (s);
        k->hit (Piece::snare, 0.9);
        k->hit (Piece::tomFloor, 0.5);
        k->hit (Piece::kick, 0.6);
    }

    const auto x = render (a, 44100, 0.5, 300);
    const auto y = render (b, 44100, 0.5, 300);
    double diff = 0, sum = 0;
    for (size_t i = 0; i < x.l.size(); ++i)
    {
        diff += (double) (x.l[i] - y.l[i]) * (x.l[i] - y.l[i]);
        sum += (double) x.l[i] * x.l[i];
    }
    CHECK (sum > 0);
    CHECK (diff == 0);
}

TEST_CASE ("Modelled Drums: a cached stroke sounds as the live one", "[drummodel]")
{
    Kit kit;
    REQUIRE (kit.load());
    kit.prepare (48000, 512);
    Settings s;
    s.room = 0;
    s.volume = 0.3;   // under the limiter
    kit.setSettings (s);

    auto waitForCache = [&kit] (Piece p, double velocity)
    {
        for (int i = 0; i < 400 && ! kit.isCached (p, velocity); ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (25));
        return kit.isCached (p, velocity);
    };

    const double velocity = 4.0 / 7.0;   // on a layer: no gain between layers

    for (const auto p : { Piece::tomMid, Piece::kick, Piece::snare, Piece::rimshot })
    {
        INFO (pieceName (p));

        kit.setCacheEnabled (false);
        kit.allNotesOff();
        kit.hit (p, velocity);
        const auto live = render (kit, 48000, 1.5);

        kit.setCacheEnabled (true);
        kit.allNotesOff();
        CHECK (! kit.isCached (p, velocity));
        kit.hit (p, velocity);   // live, and asks for it
        render (kit, 48000, 0.1);
        REQUIRE (waitForCache (p, velocity));

        kit.allNotesOff();
        kit.hit (p, velocity);
        CHECK (kit.getActivity().cymbals == 0);
        const auto cached = render (kit, 48000, 1.5);

        double diff = 0, sum = 0;
        for (size_t i = 0; i < live.l.size(); ++i)
        {
            const double dl = live.l[i] - cached.l[i], dr = live.r[i] - cached.r[i];
            diff += dl * dl + dr * dr;
            sum += (double) live.l[i] * live.l[i] + (double) live.r[i] * live.r[i];
        }
        // The snare's wires are chaotic, and its engine culls once per block:
        // rendered in other blocks than the live one's, it drifts a little.
        const bool snare = p == Piece::snare || p == Piece::rimshot;
        CHECK (sum > 0);
        CHECK (10 * std::log10 (diff / sum + 1e-30) < (snare ? -40 : -50));
    }

    CHECK (kit.getCacheBytes() > 0);

    // Turning a drum's knob throws its strokes away and renders them again
    // without another hit.
    s.toms[1].tune = 2;
    kit.setSettings (s);
    CHECK (! kit.isCached (Piece::tomMid, velocity));
    CHECK (waitForCache (Piece::tomMid, velocity));
    CHECK (kit.isCached (Piece::kick, velocity));   // the others keep theirs
}

TEST_CASE ("Modelled Drums: the resampler passes the band at unity", "[drummodel]")
{
    for (const auto& [from, to] : { std::pair { 16000.0, 44100.0 }, { 48000.0, 44100.0 }, { 48000.0, 96000.0 } })
    {
        Resampler r;
        r.prepare (from, to, 512);
        const double f = 1000.0;
        std::vector<float> outL, outR;
        double t = 0;
        for (int block = 0; block < 100; ++block)
        {
            const int n = 333;
            const int m = r.sourceNeeded (n);
            std::vector<float> inL ((size_t) m), inR ((size_t) m);
            for (int i = 0; i < m; ++i)
            {
                inL[(size_t) i] = (float) std::sin (2 * 3.141592653589793 * f * t / from);
                inR[(size_t) i] = 0;
                t += 1;
            }
            std::vector<float> l ((size_t) n), rr ((size_t) n);
            r.process (inL.data(), inR.data(), m, l.data(), rr.data(), n);
            outL.insert (outL.end(), l.begin(), l.end());
        }
        float peak = 0;
        for (size_t i = outL.size() / 2; i < outL.size(); ++i)
            peak = std::max (peak, std::abs (outL[i]));
        INFO (from << " -> " << to);
        CHECK (std::abs (20 * std::log10 (peak)) < 0.05);
    }
}
