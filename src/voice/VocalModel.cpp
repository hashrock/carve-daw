#include "VocalModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// The arithmetic below follows voice-core.js expression by expression, in the
// same order, so the two agree to rounding. This file is compiled with
// floating-point contraction off (see CMakeLists.txt): a fused multiply-add
// rounds once where the JavaScript rounds twice.
namespace carve::voice::vocal
{

namespace
{
    constexpr double RHO = 1.14, C = 350, NU = 1.5e-5;
    constexpr double PI = 3.141592653589793;
    constexpr double TAU = 2 * PI;
    constexpr double AREA = 0.7e-4;   // shape parameter (width d) -> area d^2 * AREA [m^2]
    constexpr int CTL = 32;           // control period, in steps

    // Places along the tract, glottis 0 to lips 1.
    constexpr double placePos[numPlaces] = { 1.0, 0.915, 0.875, 0.83, 0.755, 0.64, 0.585 };

    // Turbulence noise per place: [strength, high-pass Hz, low-pass Hz (0 = none)],
    // as tuned by ear in the prototype's phoneme lab.
    constexpr double noiseOf[numPlaces][3] = {
        { 4.7, 1200, 6000 }, { 2.85, 5150, 11000 }, { 2.9, 3650, 8300 }, { 6.5, 3100, 8900 },
        { 4.4, 1200, 4500 }, { 6.75, 3600, 1000 }, { 0.5, 700, 5000 }
    };

    // The nasal cavity's cross-section [cm^2], velum to nostrils.
    constexpr double NOSE[] = { 0.6, 1.4, 2.6, 3.6, 4.2, 4.0, 3.2, 2.4, 1.7, 1.2, 0.9 };
    constexpr int noseLength = (int) (sizeof (NOSE) / sizeof (NOSE[0]));

    // Equality without -Wfloat-equal's complaint; the same as == for anything but NaN.
    bool same (double a, double b)  { return ! (a < b) && ! (a > b); }

    constexpr ShapeParams REST { 0.6, 1.9, 0.25, 2.0, 2.2, 1.3 };   // the relaxed mouth

    std::array<double, N> buildShape (const ShapeParams& p)
    {
        std::array<double, N> out {};

        for (int i = 0; i < N; i++)
        {
            const double x = (double) i / (N - 1);
            const double k = std::min (1.0, std::max (0.0, (x - 0.32) / 0.2)), sm = k * k * (3 - 2 * k);
            double d = p.ph + (p.mo - p.ph) * sm;
            const double u = std::abs (x - p.tx) / p.tw;

            if (u < 1)
            {
                const double h = 0.5 + 0.5 * std::cos (PI * u);
                d -= (d - p.td) * h;
            }

            // The narrow tube of the larynx (about 2cm) widening into the pharynx.
            if (i < 5)
            {
                d = 0.8;
            }
            else if (i < 11)
            {
                const double m = (double) (i - 4) / 7;
                d = 0.8 + (d - 0.8) * m * m * (3 - 2 * m);
            }

            if (i >= N - 2)
            {
                d = std::min (d, p.lip);
            }
            else if (i >= N - 5)
            {
                const double m = (double) (i - (N - 6)) / 4;
                d = std::min (d, d + (p.lip - d) * m * m);
            }

            out[(size_t) i] = d;
        }

        return out;
    }

    // The pharynx's length against the mouth's (remapPharynx): a shape laid out
    // again along the tract, the larynx's 5 sections as they are, the pharynx
    // (to the velum) shortened or lengthened by r and the mouth taking up the
    // rest. r = 1 is the male proportion the shapes were made in; a woman's
    // pharynx is relatively shorter (0.75-0.85), a child's shorter still.
    void remapPharynx (std::array<double, N>& s, double r)
    {
        if (! (r > 0) || std::abs (r - 1) < 1e-4)
            return;

        constexpr int L0 = 5, J = noseStart, M = N - 1;
        const double ph = (J - L0) * r, P = L0 + (M - L0) * ph / (ph + (M - J));
        const auto src = s;

        for (int i = 0; i < N; i++)
        {
            const double u = i <= L0 ? i : i <= P ? L0 + (i - L0) / (P - L0) * (J - L0) : J + (i - P) / (M - P) * (M - J);
            const int k = std::min (M - 1, (int) std::floor (u));
            const double t = u - k;
            s[(size_t) i] = src[(size_t) k] + (src[(size_t) k + 1] - src[(size_t) k]) * t;
        }
    }

    std::array<double, N> toLogArea (const std::array<double, N>& d)
    {
        std::array<double, N> out {};

        for (int i = 0; i < N; ++i)
            out[(size_t) i] = std::log (d[(size_t) i] * d[(size_t) i] * AREA);

        return out;
    }

    struct Eq
    {
        double hf1, hf2, presence;
    };

    Eq eqFor (double b)
    {
        return { 3500 * std::pow (2.0, b), 4000 * std::pow (3.25, b), std::max (0.0, 0.4 + 0.25 * b) };
    }

    // The presets' tables, as tools/fit-vowels.mjs wrote them into voice-core.js.
    const VowelPreset presets[numVowelPresets] = {
        { "original", "Original", 0.5, { 1, 5, 4, 1, 3 }, {{
            {{ { 678, { 0.490, 0.670, 0.199, 1.565, 3.151, 2.569 } } }},
            {{ { 241, { 0.810, 0.670, 0.360, 2.764, 3.400, 0.880 } }, { 342, { 0.790, 0.996, 0.360, 2.642, 3.275, 1.539 } },
               { 441, { 0.754, 1.285, 0.360, 2.485, 3.134, 2.422 } }, { 540, { 0.706, 1.462, 0.352, 2.161, 3.180, 3.067 } },
               { 586, { 0.666, 1.574, 0.351, 2.227, 3.400, 3.400 } } }},
            {{ { 283, { 0.710, 3.385, 0.100, 3.000, 1.000, 0.700 } }, { 384, { 0.773, 3.400, 0.164, 2.810, 1.097, 1.078 } },
               { 479, { 0.783, 3.400, 0.246, 2.342, 1.008, 1.268 } }, { 580, { 0.790, 3.400, 0.268, 1.752, 1.000, 1.336 } } }},
            {{ { 613, { 0.615, 3.205, 0.335, 2.410, 2.640, 2.715 } } }},
            {{ { 417, { 0.495, 0.670, 0.220, 3.000, 2.675, 1.920 } }, { 518, { 0.462, 0.854, 0.230, 2.914, 2.752, 2.081 } },
               { 589, { 0.446, 0.874, 0.248, 2.686, 2.918, 2.325 } } }},
        }} },
        { "standard", "Standard", 0.5, { 1, 4, 4, 2, 2 }, {{
            {{ { 780, { 0.314, 0.670, 0.210, 1.557, 3.117, 2.736 } } }},
            {{ { 291, { 0.768, 0.670, 0.231, 2.878, 1.882, 1.355 } }, { 389, { 0.735, 0.828, 0.195, 2.456, 1.942, 1.942 } },
               { 488, { 0.703, 1.166, 0.259, 2.165, 2.260, 2.259 } }, { 585, { 0.687, 1.331, 0.329, 1.731, 3.000, 2.999 } } }},
            {{ { 330, { 0.707, 0.794, 0.175, 2.615, 2.682, 0.763 } }, { 430, { 0.661, 1.005, 0.158, 2.245, 2.839, 1.003 } },
               { 530, { 0.611, 1.104, 0.154, 1.863, 2.941, 1.242 } }, { 589, { 0.583, 1.122, 0.154, 1.652, 2.995, 1.376 } } }},
            {{ { 479, { 0.687, 1.303, 0.209, 2.591, 2.706, 1.922 } }, { 579, { 0.678, 1.684, 0.208, 2.229, 2.702, 2.284 } } }},
            {{ { 499, { 0.393, 0.670, 0.212, 2.693, 3.399, 1.095 } }, { 586, { 0.383, 0.670, 0.245, 2.816, 3.400, 1.491 } } }},
        }} },
    };

    const Character characters[numCharacters] = {
        //   key        name        tract pharynx jitter shimmer fold breath bright throat nasal press clarity body ring vib vibRate drift reg  twang reson air
        // Default: childlike -- a short tract with a high larynx (the pharynx shorter still against the
        // mouth), small light folds closing firmly, sharp resonance, little vibrato (tuned by ear).
        { "default",  "Default",    10.8, 0.69, 0,   0,   0.7,  2.6,  0.5,  0.5,   0.57, 0.82, 0.91,  0,  0,  0.5, 5.8,  1,   0.74, 0.21, 2.14, 0 },
        // Woman: the default's source on a grown-up body.
        { "woman",    "Woman",      13.5, 0.75, 0.8, 2.5, 0.75, 2.6,  0.5,  0.64,  0.81, 0.82, 0.84,  0,  0,  1,   5.8,  1,   0.67, 0.38, 1.22, 0 },
        { "standard", "Standard",   12,   0.75, 0.8, 2.5, 0.72, 1.5,  0.5,  1,     0,     0,    1,     0,  0,  1,   5.8,  1,   0,    0,    1,    0 },
        { "genki",    "Genki",      11.6, 0.75, 0.8, 2.5, 0.7,  1,    0.8,  0.9,   0.1,   0.5,  1.15, -1,  4,  0.8, 5.8,  1,   0,    0.3,  1.2,  2 },
        { "sweet",    "Sweet",      12,   0.75, 0.8, 2.5, 0.74, 3.5,  0.35, 1,     0.5,  -0.15, 0.9,   2,  0,  1.2, 5.8,  1,   0.3,  0,    1,    0 },
        { "anime",    "Anime",      11.4, 0.75, 0.8, 2.5, 0.69, 1.2,  0.7,  0.8,   0.5,   0.4,  1.05, -2,  3,  1,   5.8,  1,   0.1,  0.5,  1.2,  2 },
        { "mature",   "Mature",     13.2, 0.75, 0.8, 2.5, 0.78, 2.2,  0.3,  1.15,  0,    -0.2,  1,     3,  1,  1.3, 5.8,  1,   0,    0,    1,    0 },
        { "bell",     "Bell",       11.2, 0.75, 0.8, 2.5, 0.66, 0.4,  1,    0.85,  0.1,   0.2,  1.1,  -2,  2,  0.7, 6.3,  0.5, 0.45, 0.55, 1.8,  6 },
    };
}

const VowelPreset& getVowelPreset (int index)
{
    return presets[std::clamp (index, 0, numVowelPresets - 1)];
}

const Character& getCharacter (int index)
{
    return characters[std::clamp (index, 0, numCharacters - 1)];
}

Options Character::toOptions (Options base) const
{
    base.tractLength = tractLength / 100;
    base.pharynx = pharynx;
    base.jitter = jitter;
    base.shimmer = shimmer;
    base.fold = fold;
    base.breath = breath;
    base.brightness = brightness;
    base.throat = throat;
    base.nasal = nasal;
    base.press = press;
    base.clarity = clarity;
    base.body = body;
    base.ring = ring;
    base.vibrato = vibrato;
    base.vibratoRate = vibratoRate;
    base.drift = drift;
    base.registerMix = registerMix;
    base.twang = twang;
    base.resonance = resonance;
    base.air = air;
    return base;
}

//==============================================================================
Tract::Tract()
{
    for (int i = 0; i < NN; i++)
    {
        const double x = (i + 0.5) / NN * (noseLength - 1);
        const int j = std::min (noseLength - 2, (int) std::floor (x));
        const double t = x - j;
        nA[(size_t) i] = std::exp (std::log (NOSE[j] * 1e-4) * (1 - t) + std::log (NOSE[j + 1] * 1e-4) * t);
    }

    for (int i = 1; i < NN; i++)
        nk[(size_t) i] = (nA[(size_t) i - 1] - nA[(size_t) i]) / (nA[(size_t) i - 1] + nA[(size_t) i]);

    yNose = nA[NN - 1] / (RHO * C);
}

void Tract::setNoiseCut (double hp, double lp)
{
    nzA = 1 - std::exp (-TAU * hp / rate);
    nzB = lp > 0 ? 1 - std::exp (-TAU * lp / rate) : 1;
}

void Tract::setRate (double newRate)
{
    rate = newRate;
    fade = std::exp (-115.3 * wallLoss / rate);
    nFade = std::exp (-200 / rate);
    const double an = std::sqrt (nA[NN - 1] / PI);
    noseAl = 1 - std::exp (-TAU * std::min (0.3 * rate, C / (TAU * an)) / rate);
}

void Tract::setAreas (const double* areas, double velumArea, int steps)
{
    for (int i = 0; i < N; ++i)
        A[(size_t) i] = areas[i];

    for (int i = 1; i < N; i++)
    {
        kT[(size_t) i] = (areas[i - 1] - areas[i]) / (areas[i - 1] + areas[i]);
        dk[(size_t) i] = steps > 0 ? (kT[(size_t) i] - k[(size_t) i]) / steps : 0;
    }

    if (steps <= 0)
        k = kT;

    Z0 = RHO * C / areas[0];

    // The lips radiate like an open end with a low-pass: the smaller the
    // opening, the higher the frequencies it reflects.
    const double al = std::sqrt (areas[N - 1] / PI);
    const double lipAT = 1 - std::exp (-TAU * std::min (0.45 * rate, C / (TAU * al) * radScale) / rate);
    const double yLipT = areas[N - 1] / (RHO * C);

    if (steps > 0 && yLip > 0)
    {
        yLipMul = std::pow (yLipT / yLip, 1.0 / steps);
        lipAd = (lipAT - lipA) / steps;
    }
    else
    {
        lipA = lipAT;
        yLip = yLipT;
        yLipMul = 1;
        lipAd = 0;
    }

    const double An = std::max (1e-9, velumArea);
    constexpr int J = noseStart;
    nA[0] = An;
    nk[1] = (An - nA[1]) / (An + nA[1]);
    const double sum = areas[J - 1] + areas[J] + An;
    tl = 2 * areas[J - 1] / sum;
    tr = 2 * areas[J] / sum;
    tn = velumArea > 1e-8 ? 2 * An / sum : 0;
}

double Tract::step (double inWave, double rnd)
{
    constexpr int J = noseStart;
    jR[0] = L[0] + inWave;

    for (int i = 1; i < N; i++)
    {
        const double ki = k[(size_t) i] += dk[(size_t) i];
        const double w = ki * (R[(size_t) i - 1] - L[(size_t) i]);
        jR[(size_t) i] = R[(size_t) i - 1] + w;
        jL[(size_t) i] = L[(size_t) i] + w;
    }

    if (tn > 0)
    {
        const double pj = tl * R[J - 1] + tr * L[J] + tn * nL[0];
        jR[J] = pj - L[J];
        jL[J] = pj - R[J - 1];
        njR[0] = pj - nL[0];
    }
    else
    {
        njR[0] = -nL[0];
    }

    if (jc > 0)
    {
        // The narrowing's area, and the impedances either side, move
        // geometrically a step at a time between control ticks, rather than
        // in a staircase: a closure opening from nothing would otherwise
        // release its pressure in steps, which click. The jet's loss fades in
        // below 0.3 cm^2 instead of appearing there whole.
        jAm *= jMulA;
        jZ1 *= jMulZ1;
        jZ2 *= jMulZ2;
        const double below = std::min (1.0, (0.3e-4 - jAm) / 0.1e-4), jetShare = below <= 0 ? 0 : below * below * (3 - 2 * below);
        jRamp = std::min (1.0, jRamp + jRampStep);
        jW = std::min (1.0, std::max (0.0, jW + jWStep));
        jq = RHO / (2 * jAm * jAm) * jetShare * jRamp * jW;
        jInvA = 1 / jAm;

        // A jet at the narrowing loses the dynamic pressure (rho/2)(U/A)^2:
        // q U|U| + (Z1 + Z2) U = 2 (R - L)
        const double Rin = R[(size_t) jc - 1], Lin = L[(size_t) jc], D = 2 * (Rin - Lin), aD = D < 0 ? -D : D, Zs = jZ1 + jZ2;
        double U = 2 * aD / (Zs + std::sqrt (Zs * Zs + 4 * jq * aD));
        jVs += (U * jInvA - jVs) * 0.01;

        if (D < 0)
            U = -U;

        jR[(size_t) jc] = Lin + jZ2 * U;
        jL[(size_t) jc] = Rin - jZ1 * U;
    }

    yLip *= yLipMul;
    lipA += lipAd;
    const double lp = lipLp += (R[N - 1] - lipLp) * lipA;
    jL[N] = -lipG * lp;
    double Uo = (R[N - 1] - jL[N]) * yLip;

    for (int i = 0; i < N; i++)
    {
        R[(size_t) i] = jR[(size_t) i] * fade;
        L[(size_t) i] = jL[(size_t) i + 1] * fade;
    }

    if (fricAmp > 0)
    {
        double f = rnd * fricAmp;
        nz1 += (f - nz1) * nzA; f -= nz1;
        nz2 += (f - nz2) * nzA; f -= nz2;
        f = nz3 += (f - nz3) * nzB;
        f = nz4 += (f - nz4) * nzB;
        R[(size_t) fricPos] += f;

        if (fricPos > 0)
            L[(size_t) fricPos - 1] -= f;
    }

    for (int i = 1; i < NN; i++)
    {
        const double w = nk[(size_t) i] * (nR[(size_t) i - 1] - nL[(size_t) i]);
        njR[(size_t) i] = nR[(size_t) i - 1] + w;
        njL[(size_t) i] = nL[(size_t) i] + w;
    }

    const double nlp = noseLp += (nR[NN - 1] - noseLp) * noseAl;
    njL[NN] = -0.95 * nlp;
    Uo += (nR[NN - 1] - njL[NN]) * yNose;

    for (int i = 0; i < NN; i++)
    {
        nR[(size_t) i] = njR[(size_t) i] * nFade;
        nL[(size_t) i] = njL[(size_t) i + 1] * nFade;
    }

    return Uo;
}

void Tract::reset()
{
    R.fill (0); L.fill (0); nR.fill (0); nL.fill (0);
    lipLp = noseLp = 0;
    jVs = 0;
}

//==============================================================================
Synth::Synth (double sampleRate, const Options& options)
    : fs (sampleRate), o (options)
{
    setVowelTable (getVowelPreset (o.vowelPreset));
    shapes[vx][0] = { 560, toLogArea (buildShape (REST)) };
    shapeCount[vx] = 1;

    vCur = shapes[vx][0].s;
    vA0 = vCur;
    vA1 = vCur;

    // Where the throat knob acts: the pharynx, 10-50% of the way from the glottis.
    for (int i = 0; i < N; ++i)
    {
        const double x = (double) i / (N - 1);
        thW[(size_t) i] = x > 0.08 && x < 0.55 ? std::sin (PI * (x - 0.08) / 0.47) : 0;
    }

    // The shape of a narrowing at each place.
    const double hw = 0.06 * N;

    for (int pl = 0; pl < numPlaces; ++pl)
    {
        auto& b = bump[(size_t) pl];

        if (pl == lab)
        {
            b[N - 1] = 1; b[N - 2] = 0.7; b[N - 3] = 0.2;
            continue;
        }

        const double ci = placePos[pl] * N;
        double mx = 0;

        for (int i = 0; i < N; i++)
        {
            const double d = (i + 0.5 - ci) / hw;

            if (d > -1 && d < 1)
            {
                b[(size_t) i] = 0.5 * (1 + std::cos (PI * d));

                if (b[(size_t) i] > mx)
                    mx = b[(size_t) i];
            }
        }

        for (int i = 0; i < N; i++)
            b[(size_t) i] /= mx;
    }

    set (pitch, 60);
    set (abd, 0.05e-4);
    set (air, 1);
    set (gain, 1);

    seed = (std::uint32_t) (o.seed != 0 ? o.seed : 22222);

    setBrightness (o.brightness);
    prS = 0;
    prA = 1 - std::exp (-TAU * 2200 / fs);
    cAtk = 1 - std::exp (-1 / (0.0006 * fs));
    cRel = 1 - std::exp (-1 / (0.15 * fs));
    setBody (o.tractLength, o.fold);
    setTone();
    set (breath, 0.01e-4 * o.breath);
    control (true);
}

void Synth::setVowelTable (const VowelPreset& preset)
{
    for (int v = 0; v < 5; ++v)
    {
        shapeCount[(size_t) v] = preset.counts[(size_t) v];

        for (int j = 0; j < preset.counts[(size_t) v]; ++j)
        {
            const auto& e = preset.vowels[(size_t) v][(size_t) j];
            shapes[(size_t) v][(size_t) j] = { e.f1, toLogArea (buildShape (e.p)) };
        }
    }

    bCap.fill (0);
}

void Synth::setVowelPreset (int index)
{
    o.vowelPreset = std::clamp (index, 0, numVowelPresets - 1);
    setVowelTable (getVowelPreset (o.vowelPreset));
}

// Body (a 350Hz low shelf), ring (a 3kHz peak) and air (a 9kHz high shelf): RBJ biquads.
void Synth::setTone()
{
    auto setQ = [this] (int which, double b0, double b1, double b2, double a0, double a1, double a2)
    {
        auto& q = bq[(size_t) which];
        q[0] = b0 / a0; q[1] = b1 / a0; q[2] = b2 / a0; q[3] = a1 / a0; q[4] = a2 / a0;
    };

    {
        const double G = std::pow (10.0, o.body / 40), w = TAU * 350 / fs, cs = std::cos (w),
                     al = std::sin (w) / 2 * std::sqrt ((G + 1 / G) * (1 / 0.8 - 1) + 2), sq = 2 * std::sqrt (G) * al;
        setQ (0, G * ((G + 1) - (G - 1) * cs + sq), 2 * G * ((G - 1) - (G + 1) * cs), G * ((G + 1) - (G - 1) * cs - sq),
              (G + 1) + (G - 1) * cs + sq, -2 * ((G - 1) + (G + 1) * cs), (G + 1) + (G - 1) * cs - sq);
    }
    {
        const double G = std::pow (10.0, o.ring / 40), w = TAU * 3000 / fs, al = std::sin (w) / (2 * 1.0), cs = std::cos (w);
        setQ (1, 1 + al * G, -2 * cs, 1 - al * G, 1 + al / G, -2 * cs, 1 - al / G);
    }
    {
        const double G = std::pow (10.0, o.air / 40), w = TAU * std::min (9000.0, 0.4 * fs) / fs, cs = std::cos (w),
                     al = std::sin (w) / 2 * std::sqrt ((G + 1 / G) * (1 / 0.7 - 1) + 2), sq = 2 * std::sqrt (G) * al;
        setQ (2, G * ((G + 1) + (G - 1) * cs + sq), -2 * G * ((G - 1) + (G + 1) * cs), G * ((G + 1) + (G - 1) * cs - sq),
              (G + 1) - (G - 1) * cs + sq, 2 * ((G - 1) - (G + 1) * cs), (G + 1) - (G - 1) * cs - sq);
    }
}

void Synth::setBrightness (double b)
{
    const auto e = eqFor (std::min (2.0, std::max (-0.5, b)));
    const double top = 0.45 * fs;
    o.brightness = b;
    presence = e.presence;
    tA1 = 1 - std::exp (-TAU * std::min (top, e.hf1) / fs);
    tA2 = 1 - std::exp (-TAU * std::min (top, e.hf2) / fs);
}

double Synth::rand()
{
    std::uint32_t s = seed;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    seed = s;
    return (double) s / 2147483648.0 - 1;
}

// The body's size: the tract's length [m] and the folds' (adult male = 1).
// Scaling the folds as a similar shape, mass goes as s^3 and stiffness as s,
// so their frequency goes as 1/s.
void Synth::setBody (double len, double s)
{
    o.tractLength = len;
    o.fold = s;
    rate = N * C / len;
    dt = 1 / rate;
    ratio = rate / fs;
    tick = CTL / rate;
    tract.wallLoss = 1 / std::max (0.2, o.resonance);
    tract.radScale = o.resonance;
    tract.setRate (rate);
    aspA = 1 - std::exp (-TAU * 3500 / rate);
    bHpA = 1 - std::exp (-TAU * 1500 / rate);
    bLpA = 1 - std::exp (-TAU * 5000 / rate);

    m1b = 0.125e-3 * s * s * s; m2 = 0.025e-3 * s * s * s;
    k1 = 80 * s; k2 = 8 * s; kcb = 25 * s; r1 = 0.02 * s * s; r2 = 0.02 * s * s;
    d1b = 0.25e-2 * s; d2 = 0.05e-2 * s; lg = 1.4e-2 * s;
    m1 = m1b; kc = kcb; d1 = d1b;
    aScale = s * s; fBase = 135 / s; qa = 1.5;   // fBase: roughly the pitch at tension Q = 1
    outGain = 0.11 * std::pow (s, -1.3);
    center = 55 + 12 * std::log2 (1 / s);

    // A 2nd-order Butterworth low-pass ahead of the resampling to the output rate.
    const double fc = std::min (0.45 * fs, 16000.0), w = TAU * fc / rate, cs = std::cos (w);
    auto stage = [&] (double q) { const double al = std::sin (w) / (2 * q), a0 = 1 + al;
        return std::array<double, 5> { (1 - cs) / 2 / a0, (1 - cs) / a0, (1 - cs) / 2 / a0, -2 * cs / a0, (1 - al) / a0 }; };
    lpC = stage (0.5412);
    lpC2 = stage (1.3066);
    bCap.fill (0);
}

void Synth::setOption (Key key, double value)
{
    switch (key)
    {
        case Key::tractLength:  setBody (value, o.fold); break;
        case Key::fold:         setBody (o.tractLength, value); break;
        case Key::brightness:   setBrightness (value); break;
        case Key::body:         o.body = value; setTone(); break;
        case Key::ring:         o.ring = value; setTone(); break;
        case Key::air:          o.air = value; setTone(); break;
        case Key::registerMix:  o.registerMix = value; setBody (o.tractLength, o.fold); break;
        case Key::resonance:
            o.resonance = value;
            tract.wallLoss = 1 / std::max (0.2, value);
            tract.radScale = value;
            tract.setRate (rate);
            break;
        case Key::clarity:
            o.clarity = value;
            if (hasLastVowel)
                setVowel ((Vowel) lastVowel, 0.08, lastVowelMidi);
            break;
        case Key::breath:       o.breath = value; ramp (breath, 0.01e-4 * value, 0.1); break;
        case Key::vibrato:      o.vibrato = value; break;
        case Key::vibratoRate:  o.vibratoRate = value; break;
        case Key::drift:        o.drift = value; break;
        case Key::volume:       o.volume = value; break;
        case Key::throat:       o.throat = value; break;
        case Key::nasal:        o.nasal = value; break;
        case Key::press:        o.press = value; break;
        case Key::twang:        o.twang = value; break;
        case Key::pharynx:
            o.pharynx = value;
            if (hasLastVowel)
                setVowel ((Vowel) lastVowel, 0.08, lastVowelMidi);
            break;
        case Key::jitter:       o.jitter = value; break;
        case Key::shimmer:      o.shimmer = value; break;
    }
}

//==============================================================================
void Synth::set (Param p, double v)
{
    cur[p] = from[p] = to[p] = v;
    rt[p] = rd[p] = 0;
}

void Synth::ramp (Param p, double v, double d)
{
    if (! (d > 0))
    {
        set (p, v);
        return;
    }

    from[p] = cur[p];
    to[p] = v;
    rt[p] = 0;
    rd[p] = d;
}

// Moves to vowel v over d seconds. Given the note, it leans towards the
// variant whose first formant stays above the pitch.
void Synth::setVowel (Vowel v, double d, double midi)
{
    const int count = shapeCount[v];

    if (count == 0)
        return;

    const auto& vs = shapes[v];
    hasLastVowel = true;
    lastVowel = v;
    lastVowelMidi = midi;
    vowel = v;
    vA0 = vCur;
    vT = 0;
    vD = std::max (1e-4, ! same (d, 0) && ! std::isnan (d) ? d : 0.06);

    const double sc = 0.17 / o.tractLength;
    const double need = ! same (midi, 0) ? f1Margin * 440 * std::pow (2.0, (midi - 69) / 12) / sc : 0;
    auto& out = vA1;

    if (count == 1 || need <= vs[0].f1)
    {
        out = vs[0].s;
    }
    else
    {
        int j = 0;

        while (j < count - 2 && need > vs[(size_t) j + 1].f1)
            j++;

        const double t = std::min (1.0, std::max (0.0, (need - vs[(size_t) j].f1) / (vs[(size_t) j + 1].f1 - vs[(size_t) j].f1)));
        const auto& a = vs[(size_t) j].s;
        const auto& b = vs[(size_t) j + 1].s;

        for (int i = 0; i < N; i++)
            out[(size_t) i] = a[(size_t) i] + (b[(size_t) i] - a[(size_t) i]) * t;
    }

    // Clarity: further from the relaxed shape (> 1) or nearer it (< 1).
    const double cl = o.clarity;

    if (v != vx && ! same (cl, 1))
    {
        const auto& x = shapes[vx][0].s;
        const double lo = std::log (0.12e-4), hi = std::log (9e-4);

        for (int i = 0; i < N; i++)
            out[(size_t) i] = std::min (hi, std::max (lo, x[(size_t) i] + (out[(size_t) i] - x[(size_t) i]) * cl));
    }

    remapPharynx (out, o.pharynx);
}

void Synth::constrict (Place place, double area, double d)
{
    set (cParam (place), area);
    ramp (gParam (place), 1, d);
}

void Synth::release (Place place, double d)
{
    ramp (gParam (place), 0, d);
}

void Synth::schedule (const Event* list, int count, double offset)
{
    for (int e = 0; e < count; ++e)
    {
        if (numEvents >= queueCapacity)
            return;

        auto ev = list[e];
        ev.t += offset;

        int i = numEvents;

        while (i > 0 && events[(size_t) i - 1].t > ev.t)
            i--;

        std::memmove (events.data() + i + 1, events.data() + i, sizeof (Event) * (size_t) (numEvents - i));
        events[(size_t) i] = ev;
        ++numEvents;
    }
}

void Synth::silence()
{
    clear();
    ramp (ps, 0, 0.15);
    ramp (vib, 0, 0.1);
    ramp (air, 1, 0.05);
    ramp (gain, 1, 0.3);
    ramp (velum, 0, 0.05);
    ramp (abd, 0.05e-4, 0.05);

    for (int p = 0; p < numPlaces; ++p)
        release ((Place) p, 0.05);
}

void Synth::apply (const Event& ev)
{
    switch (ev.kind)
    {
        case Event::ramp:   ramp ((Param) ev.target, ev.value, ev.duration); break;
        case Event::vowel:  setVowel ((Vowel) ev.target, ev.duration, ev.value); break;
        case Event::close:  constrict ((Place) ev.target, ev.value, ev.duration); break;
        case Event::open:   release ((Place) ev.target, ev.duration); break;
    }
}

//==============================================================================
void Synth::control (bool init)
{
    const double now = time();

    {
        int applied = 0;

        while (applied < numEvents && events[(size_t) applied].t <= now)
            apply (events[(size_t) applied++]);

        if (applied > 0)
        {
            std::memmove (events.data(), events.data() + applied, sizeof (Event) * (size_t) (numEvents - applied));
            numEvents -= applied;
        }
    }

    for (int p = 0; p < numParams; p++)
    {
        if (rt[(size_t) p] < rd[(size_t) p])
        {
            rt[(size_t) p] += tick;
            double s = rt[(size_t) p] / rd[(size_t) p];
            if (s > 1) s = 1;
            s = s * s * (3 - 2 * s);
            cur[(size_t) p] = from[(size_t) p] + (to[(size_t) p] - from[(size_t) p]) * s;
        }
    }

    if (vT < vD)
    {
        vT += tick;
        double s = vT / vD;
        if (s > 1) s = 1;
        s = s * s * (3 - 2 * s);

        for (int i = 0; i < N; i++)
            vCur[(size_t) i] = vA0[(size_t) i] + (vA1[(size_t) i] - vA0[(size_t) i]) * s;
    }

    const double th = o.throat, tw = 1 - 0.65 * std::min (1.0, std::max (0.0, o.twang));

    for (int i = 0; i < N; i++)
        A[(size_t) i] = std::exp (vCur[(size_t) i]) * (same (th, 1) || same (thW[(size_t) i], 0) ? 1 : std::pow (th, thW[(size_t) i]));

    // Twang: narrowing the entrance of the larynx (2cm above the folds)
    // raises the singer's formant around 3kHz.
    if (! same (tw, 1))
        for (int i = 1; i < 7; i++)
            A[(size_t) i] *= std::pow (tw, i < 5 ? 1 : (7 - i) / 3.0);

    bool anyConstriction = false;
    cw.fill (0);   // per section: how far a consonant narrows it, and which place's that is

    for (int q = 0; q < numPlaces; q++)
    {
        const double g = cur[(size_t) gLab + (size_t) q];

        if (g <= 0)
            continue;

        anyConstriction = true;
        const double c = cur[(size_t) cLab + (size_t) q];
        const auto& b = bump[(size_t) q];

        for (int i = 0; i < N; i++)
            if (b[(size_t) i] * g > cw[(size_t) i])
            {
                cw[(size_t) i] = b[(size_t) i] * g;
                cq[(size_t) i] = (std::uint8_t) q;
            }

        for (int i = 0; i < N; i++)
            if (b[(size_t) i] > 0)
            {
                const double s = b[(size_t) i] * g;
                A[(size_t) i] = A[(size_t) i] * (1 - s) + c * s;
            }
    }

    anyC = anyConstriction;

    for (int i = 0; i < N; i++)
        if (A[(size_t) i] < 1e-9)
            A[(size_t) i] = 1e-9;



    // Nasality: the velum a little open during vowels only, and only once
    // the voice is going (opened before, a low い or う cannot start).
    const double vn = vowel < 5 ? o.vowelNasal[(size_t) vowel] : 1.0;
    nasV += ((anyC || voiced == 0 ? 0 : o.nasal * vn * 0.25e-4) - nasV) * (init ? 1 : voiced != 0 && ! anyC ? 0.002 : 0.15);
    tract.setAreas (A.data(), std::max (cur[velum], nasV), init ? 0 : CTL);

    if (init)
        return;

    // The narrowest point a consonant makes: the jet's resistance, and
    // turbulence from its flow. Only a consonant's narrowing makes a jet,
    // fading out as it opens (its area grows, the consonant lets go). Taking
    // the narrowest point anywhere -- a vowel's pharynx narrowed by the throat
    // knob, say -- moved the jet from the consonant into the pharynx part way
    // through a stop's release, and the pressure drop moving at once clicked
    // ("pichya" in ka, ke, ko, ta, te, to).
    int im = -1;
    double am = 1;

    for (int i = 5; i < N; i++)
        if (cw[(size_t) i] > 0 && (im < 0 || A[(size_t) i] < am))
        {
            am = A[(size_t) i];
            im = i;
        }

    double amp = 0;

    if (im >= 0 && am < 0.3e-4)
    {
        double fc = std::min (1.0, cw[(size_t) im] / 0.5);
        fc = fc * fc * (3 - 2 * fc);

        const int jc = im < N - 1 ? im + 1 : im;

        if (jc == noseStart && tract.tn > 0)
        {
            tract.jc = -1;
        }
        else
        {
            const double z1 = RHO * C / A[(size_t) jc - 1], z2 = RHO * C / A[(size_t) jc];
            if (tract.jc != jc)
            {
                if (tract.jc < 0)
                    tract.jVs = 0;

                // A jet moving to a new narrowing (from the larynx to the
                // lips, say) builds up there rather than appearing at once.
                tract.jRamp = 0;
                tract.jRampStep = 1 / (0.0015 * rate);

                tract.jc = jc;
                tract.jAm = am;
                tract.jZ1 = z1;
                tract.jZ2 = z2;
                tract.jMulA = tract.jMulZ1 = tract.jMulZ2 = 1;
                tract.jW = fc;
                tract.jWStep = 0;
            }
            else
            {
                tract.jMulA = std::pow (am / tract.jAm, 1.0 / CTL);
                tract.jMulZ1 = std::pow (z1 / tract.jZ1, 1.0 / CTL);
                tract.jMulZ2 = std::pow (z2 / tract.jZ2, 1.0 / CTL);
                tract.jWStep = (fc - tract.jW) / CTL;
            }
        }

        const double v = std::min (tract.jVs, 60.0), d = std::sqrt (4 * am / PI), Re = v * d / NU;
        fricRe = Re;

        // Only a consonant's narrowing hisses: air past the tongue in a
        // vowel must not turn into a "hi".
        if (Re > 1700 && cw[(size_t) im] > 0.25)
        {
            const auto* nz = noiseOf[cq[(size_t) im]];
            amp = fricGain * nz[0] * RHO * v * v * (1 - 1700 * 1700 / (Re * Re)) * std::min (1.0, am / 0.05e-4)
                    * std::min (1.0, (cw[(size_t) im] - 0.25) * 4);
            tract.setNoiseCut (nz[1], nz[2]);
        }

        tract.fricPos = std::min (N - 1, im + 1);
    }
    else
    {
        fricRe = 0;
        tract.jc = -1;
    }

    tract.fricAmp += (amp - tract.fricAmp) * 0.15;

    // Breathiness, kept in check: a glottal gap too wide for the folds to
    // start is narrowed until they do, and the width that worked is learnt
    // per vowel for next time.
    const double br = cur[breath], ck = br * aScale;

    if (std::abs (br - bLast) > 0.002e-4)
    {
        bLast = br;
        bCap.fill (0);
    }

    if (ck > 0.02e-4)
    {
        const double floorValue = 0.01e-4 / ck;
        const bool narrow = vowel == vi || vowel == vu || vowel == vo, high = fTarget > 2.6 * fBase;
        const auto vw = (size_t) (vowel * 2 + (high ? 1 : 0));
        const bool want = cur[ps] > 500 && cur[abd] < 0.08e-4 && ! anyC;
        const double ce = voiced != 0 ? std::min (300.0, std::abs (1200 * std::log2 (fMeas / fTarget))) : 0;
        bErr += (ce - bErr) * tick / 0.03;

        if (voiced == 0 || rt[pitch] < rd[pitch])
            bHold = 0.09;
        else if (bHold > 0)
            bHold -= tick;

        const bool strain = want && voiced != 0 && bHold <= 0 && bErr > 60;
        double capv = ! same (bCap[vw], 0) ? bCap[vw] : (narrow ? std::min (1.0, (high ? 0.015e-4 : 0.03e-4) / ck) : 1);

        if ((want && voiced == 0) || strain)
        {
            bWait += tick;

            if (bWait > (strain ? 0.05 : 0.04))
            {
                bLim = std::max (floorValue, bLim - tick / 0.06);
                bCut = true;
            }
        }
        else
        {
            bWait = 0;

            if (voiced != 0 && bCut)
            {
                bCut = false;
                bCap[vw] = capv = std::max (floorValue, bLim * 0.8);
            }
        }

        if (! bCut)
            bLim = bLim < capv ? std::min (capv, bLim + tick / 0.5) : std::max (capv, bLim - tick / 0.03);
    }
    else
    {
        bLim = 1;
        bCut = false;
        bWait = 0;
    }

    // Pitch: the target (vibrato and drift included) -> the folds' tension Q.
    vibPh += TAU * o.vibratoRate * (1 + 0.04 * std::sin (vibPh * 0.13)) * tick;

    if (vibPh > 2e4)
        vibPh -= TAU * 1000;

    // Drift: a slow wander re-aimed every 0.3-0.8s, and a fine tremble.
    driftT += (rand() - driftT) * 0.004;
    drift += (driftT - drift) * 0.02;

    if ((wTime -= tick) <= 0)
    {
        wTarget = rand();
        wTime = 0.3 + 0.25 * (rand() + 1);
    }

    wander += (wTarget - wander) * tick / 0.25;

    const double cents = cur[vib] * o.vibrato * std::sin (vibPh) + o.drift * (10 * wander + flutter * 40 * drift);
    const double ft = fTarget = 440 * std::pow (2.0, (cur[pitch] + cents / 100 - 69) / 12);

    // Jitter and shimmer: the folds' tension and the breath's pressure
    // disturbed a little each period (re-drawn as each period is measured),
    // smoothed as far as the folds can follow. The pitch is corrected towards
    // the undisturbed target ft.
    jitS += (jitT - jitS) * std::min (1.0, tick / 0.0006);
    shimS += (shimT - shimS) * std::min (1.0, tick / 0.0006);

    // Register: towards head voice the folds vibrate thinner and lighter, as
    // one piece. Low notes are kept in chest voice, as a singer does.
    const double rk = std::min (1.0, std::max (0.0, (cur[pitch] - 60) / 7)),
                 r = 0.8 * std::min (1.0, std::max (0.0, o.registerMix)) * rk * rk * (3 - 2 * rk);
    m1 = m1b * (1 - 0.45 * r);
    kc = kcb * (1 + 1.5 * r);
    d1 = d1b * (1 - 0.5 * r);

    const double q = Q = ft * (1 + jitS) / fBase * corr;

    // Tensing: mass ~ Q^-qa, stiffness ~ Q^(2-qa), damping ~ Q^(1-qa)
    const double qm = std::pow (q, qa), qk = q * q / qm, qr = q / qm;
    _m1 = dt * qm / m1;
    _m2 = dt * qm / m2;
    const double pk = 1 + 0.6 * o.press;   // press: the spring joining the two masses
    _k1 = k1 * qk;
    _k2 = k2 * qk;
    _kc = kc * qk * std::max (0.3, pk);
    _r1 = r1 * qr;
    _r2 = r2 * qr;

    lastCycAge += tick;

    if (lastCycAge > 0.03)
        voiced = 0;
}

void Synth::process (float* out, int n)
{
    const auto c = lpC;
    const double ld1 = lg * d1, ld2 = lg * d2, twoL = 2 * lg, dtl = dt, rt0 = rate;
    double X1 = x1, X2 = x2, V1 = v1, V2 = v2;

    for (int s = 0; s < n; s++)
    {
        acc += ratio;

        while (acc >= 1)
        {
            acc -= 1;

            if (stepCount == 0)
                control (false);

            if (++stepCount >= CTL)
                stepCount = 0;

            // Press: + closes the folds firmly (narrows the membranous gap,
            // abd, and the posterior chink too) and pushes harder; - the reverse.
            // Breathiness opens the posterior chink, between the cartilages, and
            // the membranous folds close up as much to keep closing each period:
            // breathy, and the harmonics still in line. (Without, the whole
            // glottis stops closing and the voice goes towards a sine.)
            const double pr = o.press, ab = cur[abd], psv = cur[ps] * (1 + 0.18 * pr) * (1 + shimS);
            const double chink = cur[breath] * aScale * bLim * (pr > 0 ? 1 - 0.85 * pr : 1 - 2 * pr);
            const double a0 = ab < 0.1e-4 ? (ab - pr * (pr > 0 ? 0.035e-4 : 0.05e-4)) * aScale - bAdd * chink : ab * aScale;

            // ---- the glottis ----
            const double a1 = a0 + twoL * X1, a2 = a0 + twoL * X2;
            const double amin = a1 < a2 ? a1 : a2;
            const double Af = (amin > 0 ? amin : 0) + chink;
            const double L0 = tract.L[0], Z0 = tract.Z0;
            const double D = psv - 2 * L0, aD = D < 0 ? -D : D;
            double U = 0;

            if (Af > 1e-10)
            {
                const double qq = RHO / (2 * Af * Af);
                U = 2 * aD / (Z0 + std::sqrt (Z0 * Z0 + 4 * qq * aD));

                if (D < 0)
                    U = -U;
            }

            const double psup = 2 * L0 + Z0 * U, dP = psv - psup;
            double P1 = 0, P2 = 0;

            if (a1 > 0)
            {
                if (amin > 0)
                {
                    const double r = amin / a1;
                    P1 = psv - dP * r * r;
                }
                else
                {
                    P1 = psv;
                }
            }

            if (a2 > 0)
                P2 = psup;

            double F1 = ld1 * P1 - _k1 * X1 - _r1 * V1 - _kc * (X1 - X2);
            double F2 = ld2 * P2 - _k2 * X2 - _r2 * V2 - _kc * (X2 - X1);

            if (a1 < 0) F1 -= 3 * _k1 * (a1 / twoL);   // the folds collide
            if (a2 < 0) F2 -= 3 * _k2 * (a2 / twoL);

            V1 += _m1 * F1; X1 += dtl * V1;
            V2 += _m2 * F2; X2 += dtl * V2;

            // The period (upward zero crossings of x1) -> the tension's correction.
            xm += (X1 - xm) * 0.0004;
            const double xc = X1 - xm;
            cyc++;

            if (xc > 0 && xPrev <= 0)
            {
                const double T = cyc - xc / (xc - xPrev);
                cyc = xc / (xc - xPrev);
                const double fm = rt0 / T, ftv = fTarget;

                if (fm > ftv * 0.6 && fm < ftv * 1.6 && psv > 100)
                {
                    fMeas = fm;
                    voiced = 1;
                    lastCycAge = 0;

                    // This period's jitter and shimmer. The folds follow about half
                    // of it, so it is doubled (tripled) to measure near the knob's %.
                    auto g = [this] { const double a = rand(), b = rand(), cc = rand(); return (a + b + cc) / 1.73; };
                    jitT = 0.02 * o.jitter * g();
                    shimT = 0.03 * o.shimmer * g();

                    // Corrected only while the mouth is open (or nasal): a
                    // consonant's load lowering the pitch is natural, and
                    // correcting for it would make the pitch jump on release.
                    if ((! anyC || cur[velum] > 0.3e-4) && cur[abd] < 0.08e-4)
                    {
                        const double cr = corr * std::exp (pitchFb * std::log (ftv / fm));
                        corr = cr < 0.5 ? 0.5 : cr > 2.5 ? 2.5 : cr;
                    }
                }
            }

            xPrev = xc;

            // Breath noise from the glottal jet, kept to about 0.8-3.5kHz.
            const double w = rand();
            nzHp += (w - nzHp) * 0.05;
            nzLp += (w - nzHp - nzLp) * aspA;
            nzLp2 += (nzLp - nzLp2) * aspA;
            const double nz = nzLp2 * 9;
            const double jet = dP < 0 ? -dP : dP;
            const double aspGain = 0.012 * (0.5 + 0.5 * o.breath);
            const double wide = cur[abd] > 0.36e-4 ? 1 + wideGain * (cur[abd] - 0.36e-4) / 0.1e-4 : 1;
            const double bs = aspGain / 0.012, airv = 1 + (cur[air] - 1) / (bs * bs * bs);
            double pn = nz * aspGain * wide * airv * jet * std::sqrt (Af < 0.3e-4 ? Af / 0.1e-4 : 3);

            // The hiss through the chink (1.5-5kHz), stronger as the membranous
            // folds open and the flow swells: in time with each period.
            if (chink > 0)
            {
                const double w2 = rand();
                bh += (w2 - bh) * bHpA;
                bl1 += (w2 - bh - bl1) * bLpA;
                bl2 += (bl1 - bl2) * bLpA;
                const double om = amin > 0 ? amin : 0;
                amPk = om > amPk ? om : amPk * 0.9997;
                const double mod = 0.3 + 0.7 * (amPk > 1e-12 ? om / amPk : 0);
                pn += bl2 * bNoise * jet * std::sqrt (chink / 0.01e-4) * mod * airv;
            }

            // ---- the tract -> radiation (the time derivative of the volume flow) ----
            const double Uo = tract.step (Z0 * U + pn, rand());
            const double vv = (Uo - Uprev) * rt0;
            Uprev = Uo;
            Ug = U;
            ga = Af;
            const double lo = c[0] * vv + lpS[0];
            lpS[0] = c[1] * vv - c[3] * lo + lpS[1];
            lpS[1] = c[2] * vv - c[4] * lo;
            double lo2 = lo;
            {
                const auto& e = lpC2;
                lo2 = e[0] * lo + lpS[2];
                lpS[2] = e[1] * lo - e[3] * lo2 + lpS[3];
                lpS[3] = e[2] * lo - e[4] * lo2;
            }
            y[0] = y[1]; y[1] = y[2]; y[2] = y[3]; y[3] = lo2;
        }

        // 4-point Hermite interpolation at the output sample's position.
        const double f = acc, ya = y[0], yb = y[1], yc = y[2], yd = y[3];
        double smp = (yb + 0.5 * f * (yc - ya + f * (2 * ya - 5 * yb + 4 * yc - yd + f * (3 * (yb - yc) + yd - ya)))) * outGain;

        // High-frequency losses of the walls, lumped into two low-passes.
        t1 += (smp - t1) * tA1;
        t2 += (t1 - t2) * tA2;
        smp = t2 * 1.5;
        prS += (smp - prS) * prA;
        smp += presence * (smp - prS);

        for (int k = 0; k < 3; k++)
        {
            const auto& q = bq[(size_t) k];
            auto& z = bz[(size_t) k];
            const double yy = q[0] * smp + q[1] * z[0] + q[2] * z[1] - q[3] * z[2] - q[4] * z[3];
            z[1] = z[0]; z[0] = smp; z[3] = z[2]; z[2] = yy;
            smp = yy;
        }

        const double h = smp - hp0 + 0.997 * hp1;
        hp0 = smp;
        hp1 = h;
        smp = h;

        // The compressor after the microphone (evens out the vowels' levels).
        const double ab = smp < 0 ? -smp : smp;
        env += (ab - env) * (ab > env ? cAtk : cRel);

        if (env > 0.06)
            smp *= std::pow (0.06 / env, 0.55);

        smp *= 1.55 * o.volume / 0.8 * cur[gain];   // gain: the singer's dynamics, after the compressor
        const double ao = smp < 0 ? -smp : smp;

        if (ao > 0.7)
            smp = (smp < 0 ? -1 : 1) * (0.7 + 0.3 * std::tanh ((ao - 0.7) / 0.3));

        out[s] = (float) smp;
        frame++;
    }

    x1 = X1; x2 = X2; v1 = V1; v2 = V2;
}

} // namespace carve::voice::vocal
