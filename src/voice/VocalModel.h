#pragma once

#include <array>
#include <cmath>
#include <cstdint>

// A physically modelled singing voice: lungs -> vocal folds -> throat, mouth
// and nose, computed a step at a time. Nothing is recorded and no waveform is
// written down anywhere: the folds oscillate by themselves (a two-mass model,
// Steinecke & Herzel 1995) in the air the lungs push through them, and the
// tract they sound into is a Kelly-Lochbaum tube of 44 sections with a nasal
// branch. Hiss and bursts come out of the turbulence of air forced through a
// narrowing, not out of a noise generator switched on for a consonant.
//
// A port of the web-voice prototype's voice-core.js (VoiceSynth, Tract, the
// vowel shape presets, the characters, the output EQ). It is meant to agree
// with that file to rounding: same arithmetic in the same order, the same
// xorshift noise, doubles throughout. The formant editing (the LM fit), the
// FFT-based formant reader and the displays are not ported -- the vowel shapes
// are the presets' tables as they stand.
//
// No JUCE, and nothing here allocates after construction: every buffer is a
// fixed array and the event queue is fixed-length, so a Synth can live on the
// audio thread. It is mono and a single voice.
namespace carve::voice::vocal
{

constexpr int N = 44;            // tract sections (glottis = 0, lips = N - 1); a wave moves one per step
constexpr int NN = 32;           // nasal sections
constexpr int noseStart = 23;    // where the nose branches off (the velum)

// Places of articulation, glottis 0 to lips 1 (PLACES in the prototype).
enum Place : std::uint8_t { lab, den, alv, apl, pal, vel, uvu, numPlaces };

// The articulators the singer moves, each with a smooth ramp of its own.
// gXxx is how far the narrowing at place Xxx is closed (0..1), cXxx the area
// it closes to.
enum Param : std::uint8_t
{
    pitch, ps, abd, velum, vib, breath,
    gLab, gDen, gAlv, gApl, gPal, gVel, gUvu,
    cLab, cDen, cAlv, cApl, cPal, cVel, cUvu,
    air, gain,
    numParams
};

constexpr Param gParam (Place p)  { return (Param) ((int) gLab + (int) p); }
constexpr Param cParam (Place p)  { return (Param) ((int) cLab + (int) p); }

// Vowel shapes. x is the relaxed mouth (between phrases).
enum Vowel : std::uint8_t { va, vi, vu, ve, vo, vx, numVowels };

// The shape of a vowel in six numbers (buildShape's arguments): where the
// tongue is highest, how narrow it is there and over how much, the width of
// the pharynx and the mouth, and the lip opening.
struct ShapeParams
{
    double tx, td, tw, ph, mo, lip;
};

// A vowel's shape, and the first formant it has at a 17cm tract. A vowel's
// second and later variants keep F2/F3 and open the mouth to raise F1, for
// high notes (formant tuning).
struct VowelVariant
{
    double f1;
    ShapeParams p;
};

constexpr int maxVariants = 4;

struct VowelPreset
{
    const char* key;
    const char* name;
    double brightness;
    std::array<int, 5> counts;                                      // variants per vowel (a i u e o)
    std::array<std::array<VowelVariant, maxVariants>, 5> vowels;
};

// The middle of a voice's comfortable range (a midi note), from its folds'
// size: what the breath is planned around (Synth::getCenter()).
inline double centerFor (double fold)  { return 55 + 12 * std::log2 (1 / fold); }

constexpr int numVowelPresets = 2;
const VowelPreset& getVowelPreset (int index);   // 0 = original (the default), 1 = standard

// The synth's settings. The defaults are DEFAULTS: neutral, the reference for
// tests and measurements. What a new instrument opens with is a Character.
struct Options
{
    double tractLength = 0.120;   // [m]
    double fold = 0.72;           // vocal-fold size (adult male = 1)
    double breath = 1.5;
    double vibrato = 1.0;
    double vibratoRate = 5.8;
    double drift = 1.0;
    double volume = 0.8;
    std::int32_t seed = 22222;
    double brightness = 0.5;
    int vowelPreset = 0;
    double throat = 1.0;
    double nasal = 0.0;
    double press = 0.0;
    double clarity = 1.0;
    double body = 0.0;
    double ring = 0.0;
    double air = 0.0;
    double registerMix = 0.0;     // "register" in the prototype
    double twang = 0.0;
    double resonance = 1.0;
};

// A voice character: a set of knob positions (CHARACTERS). tractLength is in
// cm here, as on the prototype's knob.
struct Character
{
    const char* key;
    const char* name;
    double tractLength, fold, breath, brightness, throat, nasal, press, clarity,
           body, ring, vibrato, vibratoRate, drift, registerMix, twang, resonance, air;

    Options toOptions (Options base = {}) const;
};

constexpr int numCharacters = 7;
const Character& getCharacter (int index);        // 0 = default ... 6 = bell
constexpr int defaultCharacter = 0;

// What the singer tells the body to do, at a time: ramp an articulator, move
// to a vowel, close a place, open it again.
struct Event
{
    enum Kind : std::uint8_t { ramp, vowel, close, open };

    double t = 0.0;
    Kind kind = ramp;
    std::uint8_t target = 0;   // Param for ramp, Vowel for vowel, Place for close/open
    double value = 0.0;        // ramp: the value; close: the area [m^2]; vowel: the midi note (0 = none)
    double duration = 0.0;
};

//==============================================================================
class Tract
{
public:
    Tract();

    void setNoiseCut (double hp, double lp);
    void setRate (double rate);
    void setAreas (const double* areas, double velumArea, int steps);
    double step (double inWave, double rnd);
    void reset();

    std::array<double, N> A {}, R {}, L {};
    std::array<double, N + 1> jR {}, jL {};
    std::array<double, N> k {}, kT {}, dk {};
    std::array<double, NN> nA {}, nR {}, nL {};
    std::array<double, NN + 1> njR {}, njL {};
    std::array<double, NN> nk {};

    double tl = 1, tr = 1, tn = 0;
    double lipLp = 0, noseLp = 0, lipA = 0.5, lipG = 0.995, noseAl = 0.5;
    double yLip = 0, yNose = 0, Z0 = 1;
    double yLipMul = 1, lipAd = 0;
    double fade = 0.9988, nFade = 0.997;
    int jc = -1;
    double jZ1 = 0, jZ2 = 0, jq = 0, jInvA = 0, jVs = 0;
    double jAm = 1, jMulA = 1, jMulZ1 = 1, jMulZ2 = 1;
    double jRamp = 1, jRampStep = 1;
    double fricAmp = 0;
    int fricPos = N - 1;
    double nzA = 0.1, nz1 = 0, nz2 = 0, nzB = 1, nz3 = 0, nz4 = 0;
    double rate = 1;
    double wallLoss = 1, radScale = 1;
};

//==============================================================================
class Synth
{
public:
    enum class Key { tractLength, fold, breath, vibrato, vibratoRate, drift, volume, brightness,
                     throat, nasal, press, clarity, body, ring, air, registerMix, twang, resonance };

    Synth (double sampleRate, const Options& = {});

    // Changes one setting on a running voice, as the prototype's knobs do.
    void setOption (Key, double value);
    void setVowelPreset (int index);
    const Options& getOptions() const   { return o; }

    // Queues events at their own times (plus `offset`), in time order and
    // after any already queued at the same time. Events that do not fit the
    // fixed-length queue are dropped.
    void schedule (const Event* events, int count, double offset = 0.0);
    void clear()     { numEvents = 0; }
    void silence();  // drops what is queued and stops singing, softly

    // Writes n samples (mono, overwriting).
    void process (float* out, int n);

    double time() const    { return (double) frame / fs; }
    double getCenter() const  { return center; }   // the middle of the comfortable range (a midi note)

    // The body, directly (tests, and the prototype's selftest moves).
    void set (Param, double v);
    void ramp (Param, double v, double d);
    double get (Param p) const   { return cur[p]; }
    void setVowel (Vowel, double d, double midi);
    void constrict (Place, double area, double d);
    void release (Place, double d);

    bool isVoiced() const        { return voiced != 0; }
    double getMeasuredHz() const { return fMeas; }
    double getTargetHz() const   { return fTarget; }
    double getFricationAmp() const  { return tract.fricAmp; }
    int numQueued() const        { return numEvents; }

    static constexpr int queueCapacity = 1024;

private:
    void setVowelTable (const VowelPreset&);
    void setTone();
    void setBrightness (double);
    void setBody (double len, double s);
    double rand();
    void apply (const Event&);
    void control (bool init);

    double fs;
    Options o;
    Tract tract;

    struct Shape
    {
        double f1 = 0;
        std::array<double, N> s {};
    };

    std::array<std::array<Shape, maxVariants>, numVowels> shapes {};
    std::array<int, numVowels> shapeCount {};

    std::array<double, N> vCur {}, vA0 {}, vA1 {};
    double vT = 1, vD = 1;
    int vowel = vx;
    double f1Margin = 1.3;
    bool hasLastVowel = false;
    int lastVowel = vx;
    double lastVowelMidi = 0;

    std::array<double, N> A {}, cw {};
    std::array<std::uint8_t, N> cq {};
    std::array<double, N> thW {};
    double nasV = 0;
    std::array<std::array<double, 5>, 3> bq {};
    std::array<std::array<double, 4>, 3> bz {};
    std::array<std::array<double, N>, numPlaces> bump {};

    std::array<double, numParams> cur {}, from {}, to {}, rt {}, rd {};

    double x1 = 0, x2 = 0, v1 = 0, v2 = 0;
    double Q = 1, corr = 1, pitchFb = 0.25;
    double xm = 0, xPrev = 0, cyc = 0, fMeas = 0, fTarget = 0;
    int voiced = 0;
    double lastCycAge = 0;
    double Ug = 0, ga = 0;
    bool anyC = false;
    double bLim = 1, bWait = 0;
    bool bCut = false;
    std::array<double, numVowels * 2> bCap {};   // per vowel, plain and high; 0 = not learnt
    double bLast = 0, bErr = 0, bHold = 0;
    double wideGain = 1.5, fricGain = 0.075, fricRe = 0, nzHp = 0, nzLp = 0, nzLp2 = 0, aspA = 0.2;
    double vibPh = 0, drift = 0, driftT = 0, flutter = 2.5, wander = 0, wTarget = 0, wTime = 0;
    std::uint32_t seed = 22222;

    std::array<Event, queueCapacity> events {};
    int numEvents = 0;
    std::int64_t frame = 0;
    int stepCount = 0;

    double acc = 0;
    std::array<double, 4> y {}, lpS {};
    double Uprev = 0;
    double t1 = 0, t2 = 0, presence = 0, tA1 = 0, tA2 = 0;
    double prS = 0, prA = 0;
    double hp0 = 0, hp1 = 0, env = 0, cAtk = 0, cRel = 0;

    // setBody's
    double rate = 0, dt = 0, ratio = 0, tick = 0;
    double m1b = 0, m2 = 0, k1 = 0, k2 = 0, kcb = 0, r1 = 0, r2 = 0, d1b = 0, d2 = 0, lg = 0;
    double m1 = 0, kc = 0, d1 = 0, aScale = 0, fBase = 0, qa = 1.5, outGain = 0, center = 0;
    std::array<double, 5> lpC {}, lpC2 {};

    // control()'s, for process()
    double _m1 = 0, _m2 = 0, _k1 = 0, _k2 = 0, _kc = 0, _r1 = 0, _r2 = 0;
};

} // namespace carve::voice::vocal
