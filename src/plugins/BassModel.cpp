#include "BassModel.h"

#include <algorithm>
#include <cmath>

namespace carve::plugins::bassmodel
{

namespace
{
    constexpr double tau = 6.283185307179586;
    constexpr std::array<double, numStrings> openInharmonicity { 1.3e-4, 1.0e-4, 0.75e-4, 0.55e-4 };   // roundwound strings

    bool differs (double a, double b)  { return std::abs (a - b) > 1e-12; }

    double allpassLag (double a, double w)  { return w - 2.0 * std::atan2 (a * std::sin (w), 1.0 + a * std::cos (w)); }
    double lowpassLag (double b, double w)  { return std::atan2 (b * std::sin (w), 1.0 - b * std::cos (w)); }
    double lowpassMag (double b, double w)  { return (1.0 - b) / std::sqrt (1.0 + b * b - 2.0 * b * std::cos (w)); }

    struct Coeffs { double b0, b1, b2, a1, a2; };

    Coeffs normalise (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    // RBJ biquads.
    Coeffs lowpass (double f, double q, double fs)
    {
        const double w = tau * std::min (f, 0.45 * fs) / fs, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        return normalise ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }

    Coeffs highpass (double f, double q, double fs)
    {
        const double w = tau * f / fs, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        return normalise ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }

    Coeffs peak (double f, double q, double db, double fs)
    {
        const double A = std::pow (10.0, db / 40.0), w = tau * f / fs, c = std::cos (w), al = std::sin (w) / (2.0 * q);
        return normalise (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }

    struct PresetRow { const char* name; Options o; };

    Options make (int style, double pluck, double pickup, double height, double tone, double action,
                  double sustain, double bright, double mute, double slide, double drive, double amp, double volume)
    {
        Options o;
        o.style = style; o.pluck = pluck; o.pickup = pickup; o.height = height; o.tone = tone; o.action = action;
        o.sustain = sustain; o.bright = bright; o.mute = mute; o.slide = slide; o.drive = drive; o.amp = amp; o.volume = volume;
        return o;
    }

    // style, pluck, pickup, height, tone, action, sustain, bright, mute, slide, drive, amp, volume
    const PresetRow presets[numFactoryPresets]
    {
        { "Jazz Bass, Fingers",      make (0, 0.2,  0.5,  0.5,  0.7,  0.5,  1.0, 0.5,  0.0,  0.09, 0.2,  0.4, 1.0)  },
        { "Vintage Flats",           make (0, 0.3,  0.1,  0.45, 0.35, 0.6,  0.8, 0.1,  0.35, 0.09, 0.25, 0.7, 0.42) },
        { "Bridge Growl",            make (0, 0.09, 1.0,  0.85, 0.75, 0.4,  1.2, 0.6,  0.0,  0.09, 0.3,  0.5, 0.37) },
        { "Pick Rock",               make (1, 0.12, 0.7,  0.6,  0.8,  0.45, 1.0, 0.7,  0.1,  0.09, 0.55, 0.8, 0.32) },
        { "Pick, Palm Muted",        make (1, 0.1,  0.6,  0.55, 0.6,  0.5,  0.9, 0.5,  0.7,  0.09, 0.4,  0.7, 0.47) },
        { "Slap & Pop",              make (4, 0.2,  0.45, 0.55, 0.8,  0.3,  1.1, 0.85, 0.0,  0.09, 0.1,  0.4, 0.78) },
        { "Dub",                     make (0, 0.45, 0.0,  0.5,  0.08, 0.75, 1.4, 0.05, 0.08, 0.09, 0.2,  1.0, 0.6)  },
        { "Ballad, Slow Slides",     make (0, 0.28, 0.25, 0.5,  0.55, 0.55, 1.6, 0.4,  0.0,  0.22, 0.1,  0.5, 0.85) },
        { "Fuzz",                    make (1, 0.14, 0.6,  0.7,  0.7,  0.45, 1.0, 0.6,  0.0,  0.09, 0.95, 1.0, 0.26) },
    };
}

const char* getFactoryPresetName (int index)
{
    return index >= 0 && index < numFactoryPresets ? presets[index].name : "";
}

Options getFactoryPreset (int index)
{
    return index >= 0 && index < numFactoryPresets ? presets[index].o : Options {};
}

double midiToHz (double midi)  { return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0); }

double inharmonicityOf (int string, int fret)
{
    return openInharmonicity[(size_t) string] * std::pow (2.0, 2.0 * fret / 12.0);   // inversely as the length squared
}

// The dispersion filter (M first-order allpasses sharing one coefficient),
// fitted so that partial n lands on n f0 sqrt(1 + B n^2) -- a grid search, as
// for the piano.
Dispersion designDispersion (double f1, double B, double fs, double b)
{
    constexpr double minDelay = 16.0;
    constexpr int maxStages = 8;

    const double f0 = f1 / std::sqrt (1.0 + B), w1 = tau * f1 / fs;
    const double fmax = std::min (0.42 * fs, std::max (2500.0, 24.0 * f1));

    std::array<double, 40> ws {};
    int numW = 0;

    for (int n = 1; n <= 40; ++n)
    {
        const double f = n * f0 * std::sqrt (1.0 + B * n * n);

        if (f > fmax)
            break;

        ws[(size_t) numW++] = tau * f / fs;
    }

    // RMS error of the partials' placement, or < 0 if the loop would be too short.
    auto evalFit = [&] (int M, double a)
    {
        const double D = (tau - M * allpassLag (a, w1) - lowpassLag (b, w1)) / w1;

        if (D < minDelay)
            return -1.0;

        double e = 0.0, sw = 0.0;

        for (int k = 1; k < numW; ++k)
        {
            const double w = ws[(size_t) k], n = k + 1;
            const double err = (w * D + M * allpassLag (a, w) + lowpassLag (b, w)) / tau - n, wt = 1.0 / (n * n);
            e += wt * err * err;
            sw += wt;
        }

        return sw > 0.0 ? std::sqrt (e / sw) : 0.0;
    };

    struct Candidate { int M; double a, err; };
    std::array<Candidate, 8> candidates {};
    int numCandidates = 0;

    {
        const double e = evalFit (0, 0.0);
        candidates[(size_t) numCandidates++] = { 0, 0.0, e < 0.0 ? 1e9 : e };
    }

    for (int M : { 1, 2, 3, 4, 6, 8 })
    {
        if (M > maxStages)
            break;

        Candidate best { M, 0.0, -1.0 };

        for (int k = 1; k <= 120; ++k)
        {
            const double a = -0.99 * k / 120.0, err = evalFit (M, a);

            if (err >= 0.0 && (best.err < 0.0 || err < best.err))
                best = { M, a, err };
        }

        if (best.err >= 0.0)
            candidates[(size_t) numCandidates++] = best;
    }

    double minErr = 1e9;

    for (int i = 0; i < numCandidates; ++i)
        minErr = std::min (minErr, candidates[(size_t) i].err);

    // The fewest stages that come close to the best.
    for (int i = 0; i < numCandidates; ++i)
        if (candidates[(size_t) i].err <= std::max (0.02, 1.3 * minErr))
            return { candidates[(size_t) i].M, candidates[(size_t) i].a };

    return {};
}

namespace
{
    // The loop's loss for one string and fret: T60 of the fundamental (longer
    // on the lower strings) and at 2 kHz (how new the strings are), both
    // shortened by the palm mute. Returns g and b of g (1 - b) / (1 - b z^-1).
    std::pair<double, double> designLoss (int si, int fret, double f1, double fs, const Options& o)
    {
        const double w1 = tau * f1 / fs;
        const double t0 = (7.0 + 5.5 * (1.0 - si / 3.0)) * std::pow (2.0, -fret / 24.0) * o.sustain * (1.0 - 0.93 * o.mute);
        const double th = (0.17 + 0.77 * o.bright) * o.sustain * (1.0 - 0.95 * o.mute);
        const double per0 = std::pow (10.0, -3.0 / (std::max (0.01, t0) * f1));
        const double perH = std::pow (10.0, -3.0 / (std::max (0.02, th) * f1));
        const double wh = tau * std::min (2000.0, 0.4 * fs) / fs;

        double lo = 0.0, hi = 0.999;   // b: so the 2 kHz and fundamental decays are in the right ratio

        for (int i = 0; i < 40; ++i)
        {
            const double b = 0.5 * (lo + hi);

            if (lowpassMag (b, wh) / lowpassMag (b, w1) > perH / per0)
                lo = b;
            else
                hi = b;
        }

        const double b = 0.5 * (lo + hi);
        return { std::min (0.99999, per0 / lowpassMag (b, w1)), b };
    }
}

FretDesign designFret (int si, int fret, double fs, const Options& o, const Dispersion& d)
{
    FretDesign r;
    r.f1 = midiToHz (tuning[(size_t) si] + fret);
    const double w1 = tau * r.f1 / fs;
    const auto [g, b] = designLoss (si, fret, r.f1, fs, o);
    r.g = g;
    r.b = b;
    r.stages = d.stages;
    r.a = d.a;
    // One-way delay: half the period, less the filters' phase lag at f0.
    r.rail = (fs / r.f1 - (d.stages * allpassLag (d.a, w1) + lowpassLag (b, w1)) / w1) / 2.0;
    return r;
}

//==============================================================================
// One string. The vertical (towards the frets) and horizontal vibrations are
// each two delay lines, bridge -> fret and back; the displacement x samples
// from the bridge is A[p - x] + B[p - M + x].
struct Bass::Str
{
    struct Pol
    {
        std::vector<double> A, B;
        double lp = 0.0;
        std::array<double, 8> apx {}, apy {};

        void clear()
        {
            std::fill (A.begin(), A.end(), 0.0);
            std::fill (B.begin(), B.end(), 0.0);
            lp = 0.0;
            apx.fill (0.0);
            apy.fill (0.0);
        }
    };

    // A strike in progress: a velocity pulse (thumb, hammer-on, a fret crossed).
    struct Strike { bool on = false; double x = 0.0; int n = 0, len = 0; double amp = 0.0, hor = 0.0; };

    // A fretted slide in progress: the frets still to cross.
    struct Steps { bool on = false; int n = 0, k = 0; double vel = 0.0; };

    Bass& bass;
    int si = 0;
    double fs = 48000.0;
    std::int64_t p = 0;
    std::int64_t mask = 0;
    Pol v, h;

    int fret = -1;
    FretDesign d;
    double M = -1.0, Mt = 0.0, M0 = 0.0, Mrate = 1.0, gt = 1.0;
    Steps steps;
    Strike strike;

    int held = -1;                    // the MIDI note held on it
    double damp = 0.0, dampT = 0.0;   // the fretting hand's damping when a note is let go (0 .. 1)
    double energy = 0.0;
    int quiet = 0;

    // The pickups: the displacement there before the pluck (eased back in
    // so the output does not jump), the last reading, the last flux.
    std::array<double, 2> pre {}, prevPhi {}, lastS {};

    Str (Bass& owner, int stringIndex, double sampleRate) : bass (owner), si (stringIndex), fs (sampleRate)
    {
        std::int64_t size = 1;

        while (size < (std::int64_t) (fs / midiToHz (tuning[(size_t) si]) / 2.0 + 64.0))
            size <<= 1;

        mask = size - 1;
        v.A.assign ((size_t) size, 0.0); v.B.assign ((size_t) size, 0.0);
        h.A.assign ((size_t) size, 0.0); h.B.assign ((size_t) size, 0.0);
        setFret (0, 0.0);
        M = Mt;
    }

    // glide: seconds the length takes to change. With a velocity, a fretted
    // slide: the pitch moves a semitone per fret crossed, and each crossing
    // sounds a little.
    void setFret (int newFret, double glide, double vel = 0.0)
    {
        const int from = fret;
        fret = newFret;
        d = designFret (si, fret, fs, bass.options, bass.dispersionFor (si, fret));
        Mt = d.rail;
        Mrate = glide > 0.0 ? 1.0 / (glide * fs) : 1.0;
        M0 = M < 0.0 ? Mt : M;
        gt = 0.0;
        steps = {};

        if (vel > 0.0 && from >= 0 && std::abs (fret - from) > 1)
            steps = { true, std::abs (fret - from), 0, vel };
    }

    // The loss after an option change, keeping the length where it is.
    void redesign()
    {
        d = designFret (si, fret, fs, bass.options, bass.dispersionFor (si, fret));

        const bool moving = steps.on || differs (M, Mt);
        Mt = d.rail;

        if (! moving)
            M = Mt;
    }

    double read (const std::vector<double>& buf, double pos) const
    {
        const double fl = std::floor (pos);
        const auto i = (std::int64_t) fl;
        const double f = pos - fl, a = buf[(size_t) (i & mask)];
        return a + (buf[(size_t) ((i + 1) & mask)] - a) * f;
    }

    double y (const Pol& pol, double x) const  { return read (pol.A, (double) p - x) + read (pol.B, (double) p - M + x); }

    // Adds a resting shape, shape(x / M), half to each direction.
    template <typename Shape>
    void addShape (Pol& pol, double amp, Shape&& shape)
    {
        const int n = (int) std::floor (M);
        const auto rm = (std::int64_t) std::lround (M);

        for (int x = 1; x < n; ++x)
        {
            const double s = 0.5 * amp * shape (x / M);
            pol.A[(size_t) ((p - x) & mask)] += s;
            pol.B[(size_t) ((p - rm + x) & mask)] += s;
        }
    }

    double pickupX (int k) const
    {
        return std::min (0.49, pickupPositions[(size_t) k] / std::pow (2.0, -fret / 12.0)) * M;
    }

    // The pickups take mostly the vertical vibration, a little of the horizontal.
    double pickupS (int k) const
    {
        const double xp = pickupX (k);
        return y (v, xp) + 0.1 * y (h, xp) - pre[(size_t) k];
    }

    // Finger and pick pull the string and let go (a resting displacement);
    // the thumb strikes it (displacement plus a velocity pulse); a pop pulls
    // it up and lets it slap down.
    void pluck (double vel, Style style, double pluckFrac)
    {
        const double L = scaleLength * std::pow (2.0, -fret / 12.0);
        double q = std::clamp (pluckFrac * scaleLength / L, 0.03, 0.48);

        if (style == Style::thumb)
            q = std::min (0.48, 0.27 * scaleLength / L);   // over the end of the fingerboard

        // Touching it stops what was ringing (the pickups are eased, below).
        v.clear();
        h.clear();

        // The pulled shape: a triangle, its apex rounded over the finger's (or
        // pick's, or thumb's) width.
        const double w = std::min (0.9 * std::min (q, 1.0 - q),
                                   style == Style::pick ? 0.012 : style == Style::thumb ? 0.08 : 0.05);
        auto tri = [q] (double x) { return x < q ? x / q : (1.0 - x) / (1.0 - q); };
        auto rounded = [&] (double x)
        {
            if (std::abs (x - q) >= w)
                return tri (x);

            const double u = (x - (q - w)) / (2.0 * w), y0 = tri (q - w), y2 = tri (q + w);
            return (1 - u) * (1 - u) * y0 + 2 * u * (1 - u) + u * u * y2;
        };

        const double sgn = bass.rand() < 0.5 ? -1.0 : 1.0;
        strike = {};

        if (style == Style::finger || style == Style::pick)
        {
            // A finger pushes the string towards the body as it pulls: a good
            // part vertical. A pick more so.
            const bool pick = style == Style::pick;
            const double a = vel * (pick ? 0.85 : 1.0), vert = pick ? 0.7 : 0.6, hor = pick ? 0.6 : 0.8;
            addShape (v, -a * vert, rounded);
            addShape (h, a * hor * sgn, rounded);
        }
        else if (style == Style::pop)
        {
            addShape (v, vel * 1.4, rounded);
            addShape (h, vel * 0.3 * sgn, rounded);
        }
        else
        {
            // The thumb's weight pushes the string down (the fat fundamental),
            // and the moment it lands is a short velocity pulse (the attack,
            // the string hitting the frets).
            addShape (v, -vel * 0.75, rounded);
            addShape (h, vel * 0.15 * sgn, rounded);
            strike = { true, q * M, 0, std::max (8, (int) std::lround (0.0008 * fs)), -vel * 4.0, vel * 0.3 * sgn };
        }

        // As if the finger had moved the string there gradually: what the
        // pickups jumped by is taken off and eased back in, from what they
        // last read (after a new fret, a fresh reading would differ, and the
        // step would click).
        for (int k = 0; k < 2; ++k)
        {
            const double xp = pickupX (k);
            pre[(size_t) k] = y (v, xp) + 0.1 * y (h, xp) - lastS[(size_t) k];
        }

        damp = dampT = 0.0;
        quiet = 0;
        energy = 1.0;
    }

    // A hammer-on striking the fret, or a pull-off plucking with the finger
    // that leaves.
    void tap (double vel, double x, bool down)
    {
        strike = { true, x, 0, std::max (8, (int) std::lround (0.0015 * fs)), (down ? -1.0 : 0.6) * vel * 0.5, vel * 0.1 };
        quiet = 0;
        energy = 1.0;
        damp = dampT = 0.0;
    }

    bool silent() const  { return quiet > (int) (fs * 0.2); }

    // One sample; the two pickups' outputs into out.
    void step (double* out)
    {
        const auto& o = bass.options;

        // The stopped length moving: a slide, a hammer-on, a pull-off.
        if (steps.on)
        {
            // Fretted: the finger moves smoothly but the string stops at one
            // fret after another, so the pitch steps by semitones (each step
            // eased over 1.5 ms), and the string is struck a little at every
            // fret crossed.
            gt = std::min (1.0, gt + Mrate);
            const double s = gt * gt * (3.0 - 2.0 * gt);
            const int k = std::min (steps.n, (int) std::lround (s * steps.n + 0.3));

            if (k != steps.k)
            {
                steps.k = k;
                strike = { true, M * 0.97, 0, std::max (8, (int) std::lround (0.0005 * fs)), -0.12 * steps.vel, 0.03 * steps.vel };
            }

            const double target = M0 * std::pow (Mt / M0, (double) k / steps.n);
            M += (target - M) * (1.0 - std::exp (-1.0 / (0.0015 * fs)));

            if (gt >= 1.0 && k == steps.n && std::abs (M - Mt) < 1e-3)
            {
                M = Mt;
                steps = {};
            }
        }
        else if (differs (M, Mt))
        {
            gt = std::min (1.0, gt + Mrate);
            const double s = gt * gt * (3.0 - 2.0 * gt);
            M = M0 * std::pow (Mt / M0, s);   // even in pitch

            if (gt >= 1.0)
                M = Mt;
        }

        // A note let go: the fretting finger stays on the string as it lifts,
        // and the loop loses more.
        if (dampT > damp)
            damp = std::min (dampT, damp + 1.0 / (0.012 * fs));

        const double gd = d.g * (1.0 - 0.3 * damp), bd = d.b + (1.0 - d.b) * 0.6 * damp;
        std::array<double, 2> bridge {};
        int k = 0;

        for (auto* pol : { &v, &h })
        {
            const double aOut = read (pol->A, (double) p - M), bOut = read (pol->B, (double) p - M);
            pol->B[(size_t) (p & mask)] = -aOut;   // the fret (or nut): a rigid reflection
            double x = -bOut;                      // the bridge: loss, then dispersion
            pol->lp = gd * (1.0 - bd) * x + bd * pol->lp;
            x = pol->lp;

            for (int j = 0; j < d.stages; ++j)
            {
                const double yy = d.a * x + pol->apx[(size_t) j] - d.a * pol->apy[(size_t) j];
                pol->apx[(size_t) j] = x;
                pol->apy[(size_t) j] = yy;
                x = yy;
            }

            bridge[(size_t) k++] = x;
        }

        // The saddle holds the string at a slight angle, so the two
        // vibrations pass a little into each other: whichever way it was
        // plucked, the motion grows elliptical.
        constexpr double c = 0.0005;
        v.A[(size_t) (p & mask)] = bridge[0] + c * bridge[1];
        h.A[(size_t) (p & mask)] = bridge[1] - c * bridge[0];

        // A strike in progress (a velocity pulse).
        if (strike.on)
        {
            const double t = (double) strike.n / strike.len;
            const double s = std::sin (3.141592653589793 * t) * 3.141592653589793 / (2.0 * strike.len);
            const auto xi = (std::int64_t) std::lround (std::clamp (strike.x, 1.0, M - 1.0));
            const auto rm = (std::int64_t) std::lround (M);
            v.A[(size_t) ((p - xi) & mask)] += 0.5 * strike.amp * s;
            v.B[(size_t) ((p - rm + xi) & mask)] += 0.5 * strike.amp * s;
            h.A[(size_t) ((p - xi) & mask)] += 0.5 * strike.hor * s;
            h.B[(size_t) ((p - rm + xi) & mask)] += 0.5 * strike.hor * s;

            if (++strike.n >= strike.len)
                strike = {};
        }

        // The buzz: at the three frets past the stopped one, the vertical
        // vibration is pushed back where it would go below the fret. The gap is
        // 0 at the stopped fret and the action H at the bridge (the string
        // rises straight from one to the other).
        const double H = 0.35 + 2.6 * o.action;
        const auto rm = (std::int64_t) std::lround (M);

        for (int j = 1; j <= 3 && fret + j <= numFrets; ++j)
        {
            const double r = std::pow (2.0, -j / 12.0), x = M * r, gap = H * (1.0 - r);
            const auto xr = (std::int64_t) std::lround (x);
            const auto ia = (size_t) ((p - xr) & mask), ib = (size_t) ((p - rm + xr) & mask);
            const double yy = v.A[ia] + v.B[ib];

            if (yy < -gap)
            {
                const double push = 0.5 * (-gap - yy);
                v.A[ia] += push;
                v.B[ib] += push;
            }
        }

        // The pickups: flux ~ 1/(D - s)^2, the output its rate of change.
        const double D = 1.2 + 6.0 * (1.0 - o.height);
        const double pd = std::exp (-1.0 / (0.03 * fs)), dscale = fs / 2000.0;   // the derivative is x1 at 2 kHz

        for (int kk = 0; kk < 2; ++kk)
        {
            double s = pickupS (kk);
            lastS[(size_t) kk] = s;
            pre[(size_t) kk] *= pd;
            s = std::min (s, 0.85 * D);
            const double phi = (1.0 / ((D - s) * (D - s)) - 1.0 / (D * D)) * D * D * D / 2.0;   // ~s while small
            out[kk] = (phi - prevPhi[(size_t) kk]) * dscale;
            prevPhi[(size_t) kk] = phi;
        }

        // A string not sounding stops being computed (judged by what reaches the bridge).
        const double e = std::abs (v.A[(size_t) (p & mask)]) + std::abs (h.A[(size_t) (p & mask)]);
        energy = std::max (e, energy * 0.9998);

        if (energy < 1e-6 && ! strike.on && std::abs (pre[0]) < 1e-6)
            ++quiet;
        else
            quiet = 0;

        ++p;
    }
};

//==============================================================================
double Bass::Biquad::run (double x)
{
    const double yy = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = yy;
    return yy;
}

Bass::Bass() = default;
Bass::~Bass() = default;

double Bass::rand()
{
    seedState ^= seedState << 13;
    seedState ^= seedState >> 17;
    seedState ^= seedState << 5;
    return seedState / 4294967296.0;
}

const Dispersion& Bass::dispersionFor (int string, int fret) const
{
    return dispersions[(size_t) (string * (numFrets + 1) + std::clamp (fret, 0, numFrets))];
}

void Bass::prepare (double sampleRate, std::uint32_t seed)
{
    seedState = seed != 0 ? seed : 1;

    if (! prepared || sampleRate != fs)
    {
        fs = sampleRate;
        dispersions.assign ((size_t) (numStrings * (numFrets + 1)), {});

        // The fit depends on the loss filter only through its small phase lag;
        // the default options' is close enough for every setting.
        const Options reference;

        for (int si = 0; si < numStrings; ++si)
            for (int f = 0; f <= numFrets; ++f)
            {
                const double f1 = midiToHz (tuning[(size_t) si] + f);
                const double b = designLoss (si, f, f1, fs, reference).second;
                dispersions[(size_t) (si * (numFrets + 1) + f)] = designDispersion (f1, inharmonicityOf (si, f), fs, b);
            }
    }

    prepared = true;

    for (int si = 0; si < numStrings; ++si)
        strings[(size_t) si] = std::make_unique<Str> (*this, si, fs);

    numHeld = 0;
    lastFret = 5;
    toneFilter = {};
    cab = {};
    dcX = dcY = 0.0;
    setTone();
    setCab();
}

void Bass::setOptions (const Options& o)
{
    const bool loss = differs (o.sustain, options.sustain) || differs (o.bright, options.bright) || differs (o.mute, options.mute);
    const bool tone = differs (o.tone, options.tone);
    options = o;

    if (! prepared)
        return;

    if (loss)
        redesignStrings();

    if (tone)
        setTone();
}

void Bass::redesignStrings()
{
    for (auto& s : strings)
        s->redesign();
}

// The pickups' resonance (the coil's inductance against the cable's
// capacitance) and the tone knob: a resonant second-order lowpass.
void Bass::setTone()
{
    const auto c = lowpass (900.0 * std::pow (5.5, options.tone), 0.9 + 1.2 * options.tone, fs);
    toneFilter.b0 = c.b0; toneFilter.b1 = c.b1; toneFilter.b2 = c.b2; toneFilter.a1 = c.a1; toneFilter.a2 = c.a2;
}

// The cabinet: a 33 Hz highpass (E1's 41 Hz stays), a bump at 110 Hz, a 4 kHz lowpass.
void Bass::setCab()
{
    const Coeffs cs[3] { highpass (33.0, 0.7, fs), peak (110.0, 1.0, 4.0, fs), lowpass (4000.0, 0.8, fs) };

    for (size_t k = 0; k < 3; ++k)
    {
        cab[k].b0 = cs[k].b0; cab[k].b1 = cs[k].b1; cab[k].b2 = cs[k].b2; cab[k].a1 = cs[k].a1; cab[k].a2 = cs[k].a2;
    }
}

int Bass::heldString (int midi) const
{
    for (int i = 0; i < numHeld; ++i)
        if (held[(size_t) i].first == midi)
            return held[(size_t) i].second;

    return -1;
}

void Bass::dropHeld (int midi)
{
    for (int i = 0; i < numHeld; ++i)
        if (held[(size_t) i].first == midi)
        {
            for (int j = i; j + 1 < numHeld; ++j)
                held[(size_t) j] = held[(size_t) j + 1];

            --numHeld;
            return;
        }
}

// Where to play a note: near where the hand is (the last stopped fret),
// avoiding the strings still held.
Bass::Place Bass::place (int midi) const
{
    Place best;
    double bestCost = 1e9;

    for (int si = 0; si < numStrings; ++si)
    {
        const int f = midi - tuning[(size_t) si];

        if (f < 0 || f > numFrets)
            continue;

        bool busy = false;

        for (int i = 0; i < numHeld; ++i)
            busy = busy || held[(size_t) i].second == si;

        const double cost = std::abs (f - lastFret) + (busy ? 6.0 : 0.0) + (f == 0 ? 1.5 : 0.0);

        if (cost < bestCost)
        {
            bestCost = cost;
            best = { si, f };
        }
    }

    return best;
}

void Bass::noteOn (int midi, double vel, bool legato, bool slide)
{
    if (! prepared)
        return;

    // The same note again while it is still held (on whichever string): let that go first.
    noteOff (midi);

    auto target = place (midi);

    if (target.string < 0)
        return;

    // A note held on a string that can reach this one: carry on along that string.
    if (legato || slide)
        for (int i = 0; i < numHeld; ++i)
        {
            const auto [hm, si] = held[(size_t) i];
            const int f = midi - tuning[(size_t) si];

            if (f >= 0 && f <= numFrets && hm != midi)
            {
                target = { si, f };
                break;
            }
        }

    auto& s = *strings[(size_t) target.string];
    const bool sounding = ! s.silent() && s.held >= 0;

    if ((legato || slide) && sounding)
    {
        const bool up = target.fret > s.fret;
        s.setFret (target.fret, slide ? options.slide : 0.004, slide && ! options.fretless ? vel : 0.0);

        if (! slide)
        {
            if (up)
                s.tap (vel, s.Mt, true);            // hammer-on: strikes the fret
            else
                s.tap (vel * 0.7, s.Mt * 0.95, false);   // pull-off: the leaving finger plucks
        }
    }
    else
    {
        s.setFret (target.fret, 0.0);
        s.M = s.Mt;

        auto style = (Style) std::clamp (options.style, 0, 4);

        if (style == Style::slapAndPop)
            style = target.string < 2 ? Style::thumb : Style::pop;

        s.pluck (vel, style, options.pluck);
    }

    if (s.held >= 0)
        dropHeld (s.held);

    dropHeld (midi);
    s.held = midi;

    if (numHeld < (int) held.size())
        held[(size_t) numHeld++] = { midi, target.string };

    if (target.fret > 0)
        lastFret = target.fret;
}

void Bass::noteOff (int midi)
{
    const int si = heldString (midi);

    if (si < 0)
        return;

    dropHeld (midi);
    auto& s = *strings[(size_t) si];

    if (s.held == midi)
    {
        s.held = -1;
        s.dampT = 1.0;
    }
}

void Bass::allNotesOff()
{
    while (numHeld > 0)
        noteOff (held[0].first);
}

void Bass::process (float* out, int n)
{
    if (! prepared)
    {
        std::fill_n (out, n, 0.0f);
        return;
    }

    const auto& o = options;
    const double blend = o.pickup, drive = 1.0 + 10.0 * o.drive, amp = o.amp, gain = 0.9 * o.volume;
    const double driveNorm = std::tanh (drive);
    double pk[2] {};

    for (int i = 0; i < n; ++i)
    {
        double neck = 0.0, bridge = 0.0;

        for (auto& s : strings)
        {
            if (s->silent() && s->held < 0)
            {
                ++s->p;
                continue;
            }

            s->step (pk);
            neck += pk[0];
            bridge += pk[1];
        }

        // The two pickups mixed (the bridge one is thinner and quieter, so lifted).
        double x = ((1.0 - blend) * neck + blend * 1.6 * bridge) * 2.2;
        x = toneFilter.run (x);
        dcY = 0.9995 * dcY + x - dcX;
        dcX = x;
        x = dcY;

        // The amp: drive, then the cabinet.
        double a = std::tanh (drive * x) / driveNorm;

        for (auto& f : cab)
            a = f.run (a);

        const double yy = ((1.0 - amp) * x + amp * a * 0.8) * gain;
        out[i] = (float) std::clamp (yy, -1.0, 1.0);
    }
}

Bass::StringView Bass::view (int string) const
{
    StringView r;

    if (! prepared || string < 0 || string >= numStrings)
        return r;

    const auto& s = *strings[(size_t) string];
    r.fret = s.fret;
    r.held = s.held;

    if (! s.silent())
        for (size_t k = 0; k < r.shape.size(); ++k)
        {
            const double x = ((double) k + 0.5) / 32.0 * s.M;
            r.shape[k] = (float) (s.y (s.v, x) + 0.3 * s.y (s.h, x));
        }

    return r;
}

int Bass::getSoundingCount() const
{
    int count = 0;

    if (prepared)
        for (const auto& s : strings)
            count += s->silent() ? 0 : 1;

    return count;
}

} // namespace carve::plugins::bassmodel
