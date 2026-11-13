#include "PianoModel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace carve::plugins::piano
{

namespace
{
    constexpr double tau = 6.283185307179586476925286766559;
    constexpr double pi = tau / 2.0;
    constexpr double ln10 = 2.302585092994045684017991454684;

    // Piecewise-linear interpolation through (x, y) points, extrapolated at
    // both ends.
    template <size_t N>
    double piecewise (double m, const std::array<std::array<double, 2>, N>& pts)
    {
        size_t i = 0;

        while (i < N - 2 && m > pts[i + 1][0])
            ++i;

        const auto m0 = pts[i][0], v0 = pts[i][1], m1 = pts[i + 1][0], v1 = pts[i + 1][1];
        return v0 + (v1 - v0) * (m - m0) / (m1 - m0);
    }

    // A per-key random sequence (xorshift32), the prototype's rnd(), so each
    // key gets the same unison tuning it does there.
    struct Random
    {
        explicit Random (int seed) : s ((std::uint32_t) ((std::uint64_t) (std::uint32_t) seed * 2654435761ull)) {}

        double operator()()
        {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return s / 4294967296.0;
        }

        std::uint32_t s;
    };

    // Phase lag [rad] of the first-order allpass (a + z^-1) / (1 + a z^-1).
    double apLag (double a, double w)  { return w - 2.0 * std::atan2 (a * std::sin (w), 1.0 + a * std::cos (w)); }

    // Phase lag and magnitude of the one-pole lowpass g(1-b) / (1 - b z^-1).
    double lpLag (double b, double w)  { return std::atan2 (b * std::sin (w), 1.0 - b * std::cos (w)); }
    double lpMag (double b, double w)  { return (1.0 - b) / std::sqrt (1.0 + b * b - 2.0 * b * std::cos (w)); }

    // The key's fundamental, stretch-tuned (an approximation of the Railsback
    // curve), before any unison detune.
    double fundamentalOf (int midi)
    {
        const double s = (midi - 69) / 39.0;
        const double cents = 20.0 * s * std::abs (s);
        return 440.0 * std::pow (2.0, (midi - 69) / 12.0 + cents / 1200.0);
    }

    struct Loss
    {
        double sigS, sigB, b, g1;
    };

    // The fast decay just after the strike Rp [dB/s] (energy into the bridge
    // plus the string's own losses) and the slow one after it, Rs (the
    // string's own losses alone).
    Loss lossOf (int midi, double f1, double fs)
    {
        const double Rp = 3.0 * std::pow (2.0, (midi - 36) / 16.0);
        const double Rs = 0.3 * Rp;
        const double sigS = Rs * ln10 / 20.0, sigB = (Rp - Rs) * ln10 / 20.0;

        // Higher partials die faster (sigma = sigma1 + b3 (f^2 - f1^2)),
        // capped so the gain below the fundamental (DC) never exceeds 1.
        const double b3 = std::min (2e-7 * std::pow (2.0, (midi - 60) / 20.0), 0.5 * sigS / (f1 * f1));
        const double q = 2.0 * b3 * fs * fs / (f1 * tau * tau);
        const double b = ((2.0 * q + 1.0) - std::sqrt (4.0 * q + 1.0)) / (2.0 * q);
        const double w1 = tau * f1 / fs;
        const double g1 = std::exp (-sigS / f1) / lpMag (b, w1) * (1.0 - b);   // one round trip: exp(-sigma/f1) at f1
        return { sigS, sigB, b, g1 };
    }

    int strikeDelayOf (int midi, double f1, double fs)
    {
        const double alpha = midi <= 60 ? 0.122 : 0.122 - 0.037 * (midi - 60) / 48.0;   // strike point, as a fraction of the string
        return std::max (1, (int) std::lround (alpha * (fs / f1) / 2.0));
    }

    // Per-key radiation efficiency (how much of the key the soundboard turns
    // into sound): the prototype's correction, fitted to its level checks.
    double keyGain (int midi)
    {
        static constexpr std::array<std::array<double, 2>, 6> gainDb {{ { 21, 7 }, { 36, 3 }, { 48, 0 }, { 91, 0 }, { 96, 3 }, { 108, 8 } }};
        return std::pow (10.0, piecewise (midi, gainDb) / 20.0);
    }
} // namespace

double inharmonicity (int midi)
{
    return 4e-4 * std::exp (0.0815 * (midi - 60)) + 2.5e-4 * std::exp (-0.08 * (midi - 21));
}

// The allpass cascade (M stages, one coefficient) found by grid search so
// that partial n lands at n f0 sqrt(1 + B n^2).
Dispersion designDispersion (int midi, double fs)
{
    const double f1 = fundamentalOf (midi);
    const double B = inharmonicity (midi);
    const auto loss = lossOf (midi, f1, fs);
    const int d1 = strikeDelayOf (midi, f1, fs);
    const double minDelay = 2 * d1 + 2.6;
    const int maxM = midi < 42 ? 24 : 12;

    const double f0 = f1 / std::sqrt (1.0 + B);
    const double w1 = tau * f1 / fs;
    const double fmax = std::min (0.42 * fs, std::max ({ 3000.0, std::min (16.0 * f1, 8000.0), 3.2 * f1 }));

    std::vector<double> ws;

    for (int n = 1; n <= 40; ++n)
    {
        const double f = n * f0 * std::sqrt (1.0 + B * n * n);

        if (f > fmax)
            break;

        ws.push_back (tau * f / fs);
    }

    // An allpass's lag at each partial depends on its coefficient alone, not
    // on how many there are, so it is worked out once per coefficient on the
    // grid, and every cascade length below reuses it.
    constexpr int gridSize = 160;
    const size_t np = ws.size();
    std::vector<double> lags ((gridSize + 1) * (np + 1));   // [coefficient][w1, partials...]
    std::vector<double> lowpassLags (np);

    auto gridCoefficient = [] (int k) { return -0.99 * k / (double) gridSize; };

    for (int k = 0; k <= gridSize; ++k)
    {
        const double a = gridCoefficient (k);
        auto* row = lags.data() + (size_t) k * (np + 1);
        row[0] = apLag (a, w1);

        for (size_t j = 0; j < np; ++j)
            row[j + 1] = apLag (a, ws[j]);
    }

    for (size_t j = 0; j < np; ++j)
        lowpassLags[j] = lpLag (loss.b, ws[j]);

    const double lowpassLag1 = lpLag (loss.b, w1);

    // The fit's error, in partial numbers; negative when the loop would be
    // shorter than the strike point allows.
    auto evalFit = [&] (int M, int gridIndex)
    {
        const auto* row = lags.data() + (size_t) gridIndex * (np + 1);
        const double D = (tau - M * row[0] - lowpassLag1) / w1;

        if (D < minDelay)
            return -1.0;

        double e = 0.0, sw = 0.0;

        for (size_t k = 1; k < np; ++k)
        {
            const double w = ws[k];
            const double n = (double) (k + 1);
            const double err = (w * D + M * row[k + 1] + lowpassLags[k]) / tau - n;
            const double wt = 1.0 / (n * n);
            e += wt * err * err;
            sw += wt;
        }

        return sw > 0.0 ? std::sqrt (e / sw) : 0.0;
    };

    std::vector<Dispersion> candidates;
    {
        const double e = evalFit (0, 0);
        candidates.push_back ({ 0, 0.0, e < 0.0 ? 1e9 : e });
    }

    if (np < 2)
        return candidates.front();

    for (int M : { 1, 2, 3, 4, 6, 8, 10, 12, 16, 20, 24 })
    {
        if (M > maxM)
            break;

        Dispersion best { M, 0.0, -1.0 };

        for (int k = 1; k <= gridSize; ++k)
        {
            const double err = evalFit (M, k);

            if (err >= 0.0 && (best.error < 0.0 || err < best.error))
                best = { M, gridCoefficient (k), err };
        }

        if (best.error >= 0.0)
            candidates.push_back (best);
    }

    // The fewest stages whose error is small enough (within 1.5% of a
    // partial's spacing) or within 30% of the best.
    double min = 1e300;

    for (auto& c : candidates)
        min = std::min (min, c.error);

    for (auto& c : candidates)
        if (c.error <= std::max (0.02, 1.3 * min))
            return c;

    return candidates.front();
}

KeyDesign designKey (int midi, double fs, double detune, const Dispersion& disp)
{
    KeyDesign d;
    d.midi = midi;
    d.f1 = fundamentalOf (midi);
    d.period = fs / d.f1;

    const auto loss = lossOf (midi, d.f1, fs);
    d.lowpass = loss.b;
    d.loopGain = loss.g1;
    d.dispersion = disp;

    const int ns = midi < 42 ? 2 : 3;
    d.numStrings = ns;
    const double gamma = std::sqrt (loss.sigB / (2.0 * ns * d.f1));   // coupling to the bridge
    d.strikeDelay = strikeDelayOf (midi, d.f1, fs);

    Random r (midi + 1);

    // Unison detune [cents]. Up high, once the detune in Hz outgrows the
    // bridge's coupling all that is heard is the beating, so it is capped in Hz.
    const double spread = detune * (0.35 + 0.5 * r()) * std::min (1.0, 400.0 / d.f1);
    std::array<double, maxStrings> dc {};

    if (ns == 3)
    {
        dc[0] = -spread;
        dc[1] = 0.25 * spread * (r() - 0.5);
        dc[2] = spread;
    }
    else
    {
        dc[0] = -0.5 * spread;
        dc[1] = 0.5 * spread;
    }

    // The lowest keys have one string; the second stands in for its other
    // polarisation, struck softly and coupled weakly (so it dies slowly).
    const bool mono = midi < 30;
    std::array<double, maxStrings> hw {}, gm {};

    if (mono)
    {
        hw = { 1.0, 0.3, 0.0 };
        gm = { 1.0, 0.5, 0.0 };
    }
    else if (ns == 2)
    {
        hw = { 1.04, 0.96, 0.0 };
        gm = { 1.0, 1.0, 0.0 };
    }
    else
    {
        const double h1 = 0.93 + 0.1 * r();
        const double h2 = 1.07 - 0.1 * r();
        hw = { 1.0, h1, h2 };
        gm = { 1.0, 1.0, 1.0 };
    }

    for (int i = 0; i < ns; ++i)
    {
        const double fi = d.f1 * std::pow (2.0, dc[(size_t) i] / 1200.0);
        const double w = tau * fi / fs;
        const double D = (tau - disp.stages * apLag (disp.a, w) - lpLag (loss.b, w)) / w;   // the pure delay it needs
        const int I = (int) std::floor (D - 0.5);
        const double phi = w * (D - I);                                                 // the fraction, as an allpass
        const double t = std::tan ((w - phi) / 2.0);

        auto& s = d.strings[(size_t) i];
        s.fractionalAllpass = t / (std::sin (w) - t * std::cos (w));
        s.delayOut = std::max (1, I - 2 * d.strikeDelay - 1);
        s.hammerWeight = hw[(size_t) i];
        s.gamma = gamma * gm[(size_t) i];
    }

    static constexpr std::array<std::array<double, 2>, 3> Z  {{ { 36, 3.72 },   { 60, 2.06 },    { 96, 1.97 } }};
    static constexpr std::array<std::array<double, 2>, 3> mh {{ { 36, 4.9e-3 }, { 60, 2.97e-3 }, { 96, 2.2e-3 } }};
    static constexpr std::array<std::array<double, 2>, 3> lk {{ { 36, 19.81 },  { 60, 22.23 },   { 96, 27.63 } }};
    static constexpr std::array<std::array<double, 2>, 3> p  {{ { 36, 2.3 },    { 60, 2.5 },     { 96, 3.0 } }};

    d.impedance = std::max (1.6, piecewise (midi, Z));
    d.hammerMass = std::max (1.6e-3, piecewise (midi, mh));

    // Left at the interpolated values, the top keys' contact would outlast
    // their period and they would hardly sound, so the felt hardens there.
    d.felt = std::exp (piecewise (midi, lk) + std::max (0, midi - 72) * 0.06 + std::max (0, midi - 84) * 0.1);
    d.feltExponent = piecewise (midi, p);

    const double dampT60 = std::min (0.5, std::max (0.12, 0.12 + (60 - midi) * 0.011));
    d.dampedGain = midi > 89 ? 1.0 : std::exp (-6.91 / (dampT60 * d.f1));   // the top keys have no dampers
    d.radiation = keyGain (midi);
    return d;
}

std::shared_ptr<const std::vector<Dispersion>> getDispersionTable (double sampleRate)
{
    static std::mutex lock;
    static std::map<long, std::shared_ptr<const std::vector<Dispersion>>> tables;

    const std::lock_guard<std::mutex> guard (lock);
    auto& table = tables[std::lround (sampleRate)];

    if (table == nullptr)
    {
        auto made = std::make_shared<std::vector<Dispersion>> (128);

        for (int m = lowestKey; m <= highestKey; ++m)
            (*made)[(size_t) m] = designDispersion (m, sampleRate);

        table = made;
    }

    return table;
}

//==============================================================================
// One key: its unison strings and its hammer.
struct Piano::Voice
{
    // Every buffer at the size the lowest key needs, so a voice can play any
    // key: one block, cut into each string's two delay lines.
    explicit Voice (double rate)
    {
        const auto longest = (size_t) std::ceil (rate / 26.0) + 64;
        storage.assign (maxStrings * (longest / 4 + longest), 0.0);
        auto* cursor = storage.data();

        for (size_t i = 0; i < (size_t) maxStrings; ++i)
        {
            bufL[i] = cursor;  cursor += longest / 4;
            bufO[i] = cursor;  cursor += longest;
            capL[i] = (int) (longest / 4);
            capO[i] = (int) longest;
        }
    }

    void setUp (const KeyDesign& design, std::uint32_t rev, double rate)
    {
        d = design;
        revision = rev;
        T = 1.0 / rate;
        ns = d.numStrings;
        M = d.dispersion.stages;
        a = d.dispersion.a;
        b = d.lowpass;
        g1 = d.loopGain;
        g2 = 0.0;

        for (int i = 0; i < ns; ++i)
        {
            const auto si = (size_t) i;
            const auto& s = d.strings[si];
            lenL[si] = std::min (capL[si], 2 * d.strikeDelay);
            lenO[si] = std::min (capO[si], s.delayOut);
            std::fill_n (bufL[si], lenL[si], 0.0);
            std::fill_n (bufO[si], lenO[si], 0.0);
            posL[si] = posO[si] = 0;
            af[si] = s.fractionalAllpass;
            gam[si] = s.gamma;
            hk[si] = s.hammerWeight / (2.0 * d.impedance);
            g2 += s.gamma * s.gamma;
        }

        ap.fill (0.0);
        lp.fill (0.0);
        sf.fill (0.0);
        vp.fill (0.0);
        gd = gdT = 1.0;
        hOn = false;
        yh = vh = ys = 0.0;
        touched = false;
        K = d.felt;
        p = d.feltExponent;
        invMass = 1.0 / d.hammerMass;
        invZ2 = 1.0 / (2.0 * d.impedance);
        cH = T * T / d.hammerMass + T / (2.0 * d.impedance);
        acc = 0.0;
        cnt = 0;
        level = 1.0;
        dead = false;
        held = false;
        age = 0;
        minAge = 0.2 * rate + 4.0 * d.period;
        F = 0.0;

        // Panned by where the key is (bass on the player's left).
        const double th = (std::max (-1.0, std::min (1.0, (d.midi - 64) / 60.0)) * 0.5 + 0.5) * pi / 2.0;
        panL = std::cos (th) * 1.414 * d.radiation;
        panR = std::sin (th) * 1.414 * d.radiation;
    }

    // A strike. A string still ringing can be struck again, as it is.
    void strike (double v0, double hardness)
    {
        K = d.felt * hardness;
        yh = 0.0;
        ys = 0.0;
        vh = v0;
        hOn = true;
        touched = false;
        gdT = 1.0;
        gd = 1.0;
        held = true;
        dead = false;
        level = 1.0;
        age = 0;
    }

    void release()
    {
        held = false;
        gdT = d.dampedGain;
    }

    double step (double vhatIn)  { return ns == 3 ? stepWith<3> (vhatIn) : stepWith<2> (vhatIn); }

    // One sample. vhatIn is the bridge's velocity a sample ago (normalised);
    // the return value is sum(gamma_i v_i+).
    //
    // The bridge is one point all the strings share, a resistance Zb. With
    // string i's coupling gamma_i = c_i sqrt(Z_i / Zb) and its incoming wave
    // v_i+, the scattering
    //     vhat = 2 sum(gamma_i v_i+) / (1 + sum(gamma_i^2)),   v_i- = gamma_i vhat - v_i+
    // is passive. Only what the strings do in phase leaves through the
    // bridge, which with the unison strings' slight detune gives the fast
    // decay, then the slow one, and the beating.
    //
    // NS is the number of strings, fixed so the strings' work is unrolled and
    // their allpass cascades (independent of each other) run side by side.
    template <int NS>
    double stepWith (double vhatIn)
    {
        double vr[NS], vl[NS], x[NS];
        double vfree = 0.0;

        for (int i = 0; i < NS; ++i)
        {
            vr[i] = gam[(size_t) i] * vhatIn - vp[(size_t) i];
            vl[i] = -bufL[(size_t) i][posL[(size_t) i]];   // the agraffe end is fixed (inverting)
            vfree += vr[i] + vl[i];
        }

        double force = 0.0;
        F = 0.0;

        if (hOn)
            force = hammer (vfree / NS);

        if (std::abs (gd - gdT) > 0.0)
        {
            gd += (gdT - gd) * 0.002;

            if (std::abs (gd - gdT) < 1e-7)
                gd = gdT;
        }

        for (int i = 0; i < NS; ++i)
        {
            const auto si = (size_t) i;
            const double u = force * hk[si];

            int q = posL[si];
            bufL[si][q] = vr[i] + u;
            posL[si] = ++q == lenL[si] ? 0 : q;

            q = posO[si];
            x[i] = bufO[si][q];
            bufO[si][q] = vl[i] + u;
            posO[si] = ++q == lenO[si] ? 0 : q;
        }

        // The dispersion: M first-order allpasses, all strings at once.
        double* st = ap.data();

        for (int k = 0; k < M; ++k, st += NS)
        {
            for (int i = 0; i < NS; ++i)
            {
                const double y = a * x[i] + st[i];
                st[i] = x[i] - a * y;
                x[i] = y;
            }
        }

        double S = 0.0, sumAbs = 0.0;

        for (int i = 0; i < NS; ++i)
        {
            const auto si = (size_t) i;
            const double l = lp[si] = g1 * x[i] + b * lp[si];
            const double y = af[si] * l + sf[si];
            sf[si] = l - af[si] * y;
            const double v = y * gd;

            vp[si] = v;
            S += gam[si] * v;
            sumAbs += std::abs (v);
        }

        acc += sumAbs;

        if (++cnt == 512)
        {
            level = acc / (512.0 * NS);
            acc = 0.0;
            cnt = 0;
            age += 512;

            if (! hOn && ! held && level < 3e-7 && (double) age > minAge)
                dead = true;
        }

        return S;
    }

    // The hammer's contact with the string, solved implicitly for the felt's
    // compression x: x + c K x^p = x0. Returns the force on the string.
    double hammer (double vfree)
    {
        double force = 0.0;
        const double x0 = (yh + T * vh) - (ys + T * vfree);

        if (x0 > 0.0)
        {
            const double cK = cH * K;
            double x = x0, xp = 0.0;

            for (int it = 0; it < 8; ++it)
            {
                xp = std::exp ((p - 1.0) * std::log (x));   // x^(p-1)
                const double f = x + cK * xp * x - x0;
                const double nx = x - f / (1.0 + cK * p * xp);

                if (std::abs (nx - x) < 1e-12)
                {
                    x = nx;
                    break;
                }

                x = nx > 0.0 ? nx : x * 0.5;
            }

            force = F = K * std::exp (p * std::log (x));
            touched = true;
        }
        else if (touched && vh < 0.0)
        {
            hOn = false;   // bounced off
        }

        vh -= T * force * invMass;
        yh += T * vh;
        ys += T * (vfree + force * invZ2);
        return force;
    }

    KeyDesign d;
    std::uint32_t revision = 0;
    double T = 1.0 / 44100.0;
    int ns = 0, M = 0;
    double a = 0.0, b = 0.0, g1 = 0.0, g2 = 0.0;

    std::vector<double> storage;
    std::array<double*, maxStrings> bufL {}, bufO {};
    std::array<int, maxStrings> capL {}, capO {}, lenL {}, lenO {}, posL {}, posO {};
    std::array<double, maxStrings * maxAllpasses> ap {};
    std::array<double, maxStrings> lp {}, sf {}, vp {}, af {}, gam {}, hk {};

    double gd = 1.0, gdT = 1.0;
    bool hOn = false, touched = false;
    double yh = 0.0, vh = 0.0, ys = 0.0;
    double K = 0.0, p = 2.5, cH = 0.0, invMass = 0.0, invZ2 = 0.0;
    double acc = 0.0;
    int cnt = 0;
    double level = 1.0;
    bool dead = false, held = false;
    std::int64_t age = 0;
    double minAge = 0.0;
    double F = 0.0;   // the hammer's force now [N], for the knock
    double panL = 0.0, panR = 0.0;
};

//==============================================================================
// A feedback delay network (eight lines, a Householder matrix). Short, it is
// the soundboard's dense resonance; long, the room.
struct Piano::Fdn
{
    Fdn (double rate, const std::array<double, 8>& delaysMs, double t60, double t60hf, int seed)
    {
        Random rnd (seed);

        for (size_t i = 0; i < n; ++i)
        {
            const int length = std::max (2, (int) std::lround (delaysMs[i] * rate / 1000.0));
            buf[i].assign ((size_t) length, 0.0);
            const double g0 = std::pow (10.0, -3.0 * length / (t60 * rate));
            const double gh = std::pow (10.0, -3.0 * length / (t60hf * rate));
            const double coeff = std::min (0.95, std::max (0.0, 1.0 - gh / g0));   // the highs die faster, through a one-pole lowpass
            c[i] = coeff;
            g[i] = g0 * (1.0 - coeff);
            sl[i] = rnd() < 0.5 ? -1.0 : 1.0;
            sr[i] = rnd() < 0.5 ? -1.0 : 1.0;
            inp[i] = (rnd() < 0.5 ? -1.0 : 1.0) / std::sqrt ((double) n);
        }
    }

    void clear()
    {
        for (auto& b : buf)
            std::fill (b.begin(), b.end(), 0.0);

        lp.fill (0.0);
        pos.fill (0);
        l = r = 0.0;
    }

    void tick (double x)
    {
        double sum = 0.0, left = 0.0, right = 0.0;

        for (size_t i = 0; i < n; ++i)
        {
            const double v = lp[i] = g[i] * buf[i][(size_t) pos[i]] + c[i] * lp[i];
            o[i] = v;
            sum += v;
            left += v * sl[i];
            right += v * sr[i];
        }

        sum *= 2.0 / n;

        for (size_t i = 0; i < n; ++i)
        {
            auto& b = buf[i];
            int p = pos[i];
            b[(size_t) p] = o[i] - sum + x * inp[i] + 1e-25;
            if (++p == (int) b.size()) p = 0;
            pos[i] = p;
        }

        l = left;
        r = right;
    }

    static constexpr size_t n = 8;
    std::array<std::vector<double>, n> buf;
    std::array<int, n> pos {};
    std::array<double, n> g {}, c {}, lp {}, sl {}, sr {}, inp {}, o {};
    double l = 0.0, r = 0.0;
};

//==============================================================================
// Two-pole resonators with a peak gain of 1: the soundboard's low modes.
struct Piano::Modes
{
    struct Mode { double f, t60, gl, gr; };

    // A fixed count, in fours, so the resonators run four at a time.
    static constexpr size_t n = 28;

    explicit Modes (double rate, const std::array<Mode, n>& list)
    {
        for (size_t i = 0; i < n; ++i)
        {
            const auto& m = list[i];
            const double radius = std::exp (-6.91 / (m.t60 * rate));
            c1[i] = 2.0 * radius * std::cos (tau * m.f / rate);
            c2[i] = -radius * radius;
            b0[i] = (1.0 - radius * radius) / 2.0;
            gl[i] = m.gl;
            gr[i] = m.gr;
        }

        clear();
    }

    void clear()
    {
        y1.fill (0.0);
        y2.fill (0.0);
        x1 = x2 = l = r = 0.0;
    }

    void tick (double x)
    {
        const double d = x - x2 + 1e-25;
        x2 = x1;
        x1 = x;
        double left[4] {}, right[4] {};

        for (size_t i = 0; i < n; i += 4)
        {
            for (size_t j = 0; j < 4; ++j)
            {
                const double y = b0[i + j] * d + c1[i + j] * y1[i + j] + c2[i + j] * y2[i + j];
                y2[i + j] = y1[i + j];
                y1[i + j] = y;
                left[j] += y * gl[i + j];
                right[j] += y * gr[i + j];
            }
        }

        l = (left[0] + left[1]) + (left[2] + left[3]);
        r = (right[0] + right[1]) + (right[2] + right[3]);
    }

    static_assert (n % 4 == 0);
    alignas (32) std::array<double, n> c1 {}, c2 {}, b0 {}, gl {}, gr {}, y1 {}, y2 {};
    double x1 = 0.0, x2 = 0.0, l = 0.0, r = 0.0;
};

//==============================================================================
// The open strings that ring along while the pedal is down: the fourteen
// lowest, each a delay-line loop, so they take up whole harmonic series
// rather than one frequency each.
struct Piano::Sympathetic
{
    struct String
    {
        std::vector<double> buf;
        int i = 0;
        double z = 0.0, gOpen = 0.0, gShut = 0.0, l = 0.0, r = 0.0;
    };

    explicit Sympathetic (double rate)
    {
        static constexpr int keys[] { 28, 29, 31, 33, 34, 36, 38, 40, 41, 43, 45, 46, 48, 50 };
        int k = 0;

        for (int m : keys)
        {
            const double f = 440.0 * std::pow (2.0, (m - 69) / 12.0);
            String s;
            s.buf.assign ((size_t) std::max (1L, std::lround (rate / f - 0.5)), 0.0);
            s.gOpen = std::pow (10.0, -3.0 / (f * 5.0));
            s.gShut = std::pow (10.0, -3.0 / (f * 0.25));
            const double sign = (k & 1) ? -1.0 : 1.0;
            s.l = (k % 3 == 0 ? 1.0 : 0.4) * sign;
            s.r = (k % 3 == 2 ? 1.0 : 0.4) * sign;
            strings.push_back (std::move (s));
            ++k;
        }
    }

    void clear()
    {
        for (auto& s : strings)
        {
            std::fill (s.buf.begin(), s.buf.end(), 0.0);
            s.i = 0;
            s.z = 0.0;
        }

        l = r = 0.0;
        alive = false;
    }

    void tick (double x, double open)   // open: how far the dampers are up, 0..1
    {
        double left = 0.0, right = 0.0, energy = 0.0;

        for (auto& s : strings)
        {
            const double o = s.buf[(size_t) s.i];
            const double f = (o + s.z) * 0.5;
            s.z = o;
            s.buf[(size_t) s.i] = x + f * (s.gShut + (s.gOpen - s.gShut) * open) + 1e-24;
            if (++s.i >= (int) s.buf.size()) s.i = 0;
            left += o * s.l;
            right += o * s.r;
            energy += std::abs (o);
        }

        l = left;
        r = right;
        alive = energy > 1e-9;
    }

    std::vector<String> strings;
    double l = 0.0, r = 0.0;
    bool alive = false;
};

//==============================================================================
Piano::Piano() = default;
Piano::~Piano() = default;

void Piano::prepare (double sampleRate)
{
    if (prepared && std::abs (sampleRate - fs) < 1.0e-6)
    {
        allNotesOff();
        return;
    }

    fs = sampleRate;
    dispersions = getDispersionTable (fs);
    designedDetune = -1.0;
    ++revision;

    voices.clear();

    for (int i = 0; i < maxVoices; ++i)
        voices.push_back (std::make_unique<Voice> (fs));

    hpc = std::exp (-tau * 1500.0 / fs);
    rc = std::exp (-tau * 70.0 / fs);   // the soundboard radiates little of the lowest notes (a 70Hz high-pass)

    Random r (7);
    std::array<Modes::Mode, Modes::n> low {};

    for (int k = 0; k < (int) Modes::n; ++k)        // the soundboard's low modes, 55Hz to about 700Hz
    {
        const double f = 55.0 * std::pow (700.0 / 55.0, (k + 0.8 * (r() - 0.5)) / 27.0);
        const double pan = 0.5 + 0.7 * (r() - 0.5);
        const double sg = r() < 0.5 ? -1.0 : 1.0;
        low[(size_t) k] = { f, std::min (0.6, std::max (0.12, 0.55 * std::pow (f / 80.0, -0.5))),
                            sg * std::cos (pan * pi / 2.0), sg * std::sin (pan * pi / 2.0) };
    }

    modes = std::make_unique<Modes> (fs, low);
    board = std::make_unique<Fdn> (fs, std::array<double, 8> { 2.03, 2.71, 3.37, 4.19, 5.33, 6.47, 7.93, 9.61 }, 0.3, 0.09, 11);
    room = std::make_unique<Fdn> (fs, std::array<double, 8> { 29.3, 37.1, 41.9, 47.3, 53.9, 61.7, 71.3, 79.9 }, 1.7, 0.6, 23);
    sym = std::make_unique<Sympathetic> (fs);
    preDelay.assign ((size_t) std::lround (0.014 * fs), 0.0);   // the room's first reflections come back 14ms late

    prepared = true;
    allNotesOff();
    setOptions (options);
}

void Piano::setOptions (const Options& o)
{
    options = o;

    if (prepared && std::abs (options.detune - designedDetune) > 0.0)
    {
        // Cheap: the dispersion is already designed, so this is a few
        // dozen operations a key.
        designedDetune = options.detune;
        ++revision;

        for (int m = lowestKey; m <= highestKey; ++m)
        {
            designs[(size_t) m] = designKey (m, fs, designedDetune, (*dispersions)[(size_t) m]);
            designRevision[(size_t) m] = revision;
        }
    }
}

const KeyDesign& Piano::designFor (int midi)
{
    return designs[(size_t) midi];
}

void Piano::recomputeCoupling()
{
    couplingSum = 0.0;

    for (int i = 0; i < numActive; ++i)
        couplingSum += active[(size_t) i]->g2;
}

void Piano::drop (Voice* v)
{
    for (int i = 0; i < numActive; ++i)
    {
        if (active[(size_t) i] == v)
        {
            std::move (active.begin() + i + 1, active.begin() + numActive, active.begin() + i);
            --numActive;
            break;
        }
    }

    if (byKey[(size_t) v->d.midi] == v)
        byKey[(size_t) v->d.midi] = nullptr;

    recomputeCoupling();
}

void Piano::steal()
{
    Voice* best = nullptr;
    double score = 1e300;

    for (int i = 0; i < numActive; ++i)
    {
        auto* v = active[(size_t) i];
        const double s = v->level * (v->held ? 100.0 : 1.0);

        if (s < score)
        {
            score = s;
            best = v;
        }
    }

    if (best != nullptr)
        drop (best);
}

Piano::Voice* Piano::startVoice (int midi)
{
    if (numActive >= maxVoices)
        steal();

    // A voice nobody is using.
    Voice* free = nullptr;

    for (auto& v : voices)
    {
        bool inUse = false;

        for (int i = 0; i < numActive && ! inUse; ++i)
            inUse = active[(size_t) i] == v.get();

        if (! inUse)
        {
            free = v.get();
            break;
        }
    }

    if (free == nullptr)
        return nullptr;

    free->setUp (designFor (midi), designRevision[(size_t) midi], fs);
    byKey[(size_t) midi] = free;
    active[(size_t) numActive++] = free;
    couplingSum += free->g2;
    return free;
}

void Piano::noteOn (int midi, double velocity)
{
    if (! prepared || midi < lowestKey || midi > highestKey)
        return;

    const double v = std::min (1.0, std::max (0.02, velocity));
    const double v0 = 0.25 + 4.75 * v * v;   // hammer speed [m/s]

    auto* voice = byKey[(size_t) midi];

    if (voice == nullptr || voice->revision != designRevision[(size_t) midi])
    {
        if (voice != nullptr)
            drop (voice);

        voice = startVoice (midi);
    }

    if (voice != nullptr)
    {
        voice->strike (v0, options.hardness);
        asleep = false;
        quietSamples = 0;
    }
}

void Piano::noteOff (int midi)
{
    if (midi < 0 || midi > 127)
        return;

    if (auto* v = byKey[(size_t) midi])
    {
        v->held = false;

        if (! sustain)
            v->release();
    }
}

void Piano::setPedal (bool down)
{
    sustain = down;

    if (! down)
        for (int i = 0; i < numActive; ++i)
            if (! active[(size_t) i]->held)
                active[(size_t) i]->release();
}

void Piano::allNotesOff()
{
    numActive = 0;
    byKey.fill (nullptr);
    couplingSum = 0.0;
    vhat = 0.0;
    sustain = false;
    symGain = 0.0;
    knockLp = 0.0;

    if (prepared)
    {
        modes->clear();
        board->clear();
        room->clear();
        sym->clear();
        std::fill (preDelay.begin(), preDelay.end(), 0.0);
    }

    preDelayPos = 0;
    hp1 = hp2 = rl = rr = 0.0;
    asleep = true;   // all silent, so nothing to compute until a key is struck
    quietSamples = 0;
}

void Piano::process (float* left, float* right, int n)
{
    // Nothing is sounding, the room included: skip it all. A piano left on a
    // track that is not playing costs nothing.
    if (! prepared || asleep)
        return;

    const auto& o = options;
    const double vol = o.volume * 1.5, bd = o.board, rv = o.reverb, sy = o.sympathetic * 0.012,
                 br = o.brightness * 2.0, kn = o.knock * 3e-3;
    const double symT = sustain ? 1.0 : 0.0;
    const int pdLen = (int) preDelay.size();
    int lastHeard = -1;   // the last sample above -120dB

    for (int s = 0; s < n; ++s)
    {
        double S = 0.0, OL = 0.0, OR = 0.0, knock = 0.0;
        const double vh = vhat;

        for (int k = 0; k < numActive; ++k)
        {
            auto* nt = active[(size_t) k];
            const double c = nt->step (vh);
            S += c;
            OL += c * nt->panL;
            OR += c * nt->panR;
            knock += nt->F * nt->d.radiation;
        }

        const double norm = 2.0 / (1.0 + couplingSum);
        vhat = S * norm;
        const double xl = OL * norm, xr = OR * norm, x = (xl + xr) * 0.5;

        // The knock: the force of the hammer on the string, through the
        // bridge into the soundboard.
        knockLp += (knock * kn - knockLp) * 0.25;

        // Open strings ringing along.
        symGain += (symT - symGain) * 0.0006;
        double syL = 0.0, syR = 0.0;

        if (symGain > 1e-4 || sym->alive)
        {
            sym->tick (x * symGain, symGain);
            syL = sym->l * sy;
            syR = sym->r * sy;
        }

        // Soundboard: the direct sound, the low modes, the dense resonance.
        const double xb = x + knockLp + (syL + syR) * 0.5;
        modes->tick (xb);
        board->tick (xb);
        double l = xl * 0.75 + syL * 0.5 + bd * (0.55 * modes->l + 0.5 * board->l);
        double r = xr * 0.75 + syR * 0.5 + bd * (0.55 * modes->r + 0.5 * board->r);

        // Brightness: a lift from 1.5kHz up.
        hp1 += (l - hp1) * (1.0 - hpc);
        l += br * (l - hp1);
        hp2 += (r - hp2) * (1.0 - hpc);
        r += br * (r - hp2);
        rl += (l - rl) * (1.0 - rc);
        l -= rl;
        rr += (r - rr) * (1.0 - rc);
        r -= rr;

        // The room, 14ms behind.
        const double delayed = preDelay[(size_t) preDelayPos];
        preDelay[(size_t) preDelayPos] = (l + r) * 0.5;
        if (++preDelayPos >= pdLen) preDelayPos = 0;
        room->tick (delayed);
        l = (l + rv * room->l) * vol;
        r = (r + rv * room->r) * vol;

        // Output protection: only the loud parts are rounded off.
        auto soft = [] (double v)
        {
            const double av = std::abs (v);
            return av > 0.7 ? std::copysign (0.7 + 0.3 * std::tanh ((av - 0.7) / 0.3), v) : v;
        };

        if (std::abs (l) >= 1e-6 || std::abs (r) >= 1e-6)
            lastHeard = s;

        left[s] += (float) soft (l);
        right[s] += (float) soft (r);
    }

    for (int i = numActive; --i >= 0;)
        if (active[(size_t) i]->dead)
            drop (active[(size_t) i]);

    // Once every voice is gone and the room has rung down to -120dB for a
    // quarter of a second, sleep. What is left of the tail is far below
    // hearing, and picks up from where it was when a key wakes it.
    if (numActive > 0 || sym->alive)
        quietSamples = 0;
    else
        quietSamples = lastHeard < 0 ? quietSamples + n : n - 1 - lastHeard;

    if (quietSamples >= (int) (0.25 * fs))
        asleep = true;
}

} // namespace carve::plugins::piano
