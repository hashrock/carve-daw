#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace carve::plugins::bass
{

// The electric bass's sound engine: a port of web-bass's bass-decoder.js
// (a sibling prototype of this app), kept step for step so the two render the
// same samples. A note is
//
//   harmonics   the string's partials, each with its own envelope (24 points
//               in time, dB), plus a second, exactly harmonic comb -- the
//               fret buzz -- with envelopes of its own;
//   transient   the first few periods of the recording that the partials do
//               not account for, kept as PCM.
//
// Fourteen pitches by four dynamics were analysed; every note in between is
// interpolated from its neighbours (in dB, partial by partial), which is why
// the kit is 400KB rather than a sample library.
//
// Real-time use: reserve() a Spec and each Voice once, off the audio thread.
// After that makeSpec(), Voice::start() and Voice::render() allocate nothing.

constexpr int maxStringPartials = 512;
constexpr int maxBuzzPartials = 512;

struct Note
{
    int pitchIndex = 0, layerIndex = 0, ref = 0;
    double midi = 0.0, f0 = 0.0, B = 0.0, top = 0.0, ta = 0.0, tscale = 0.0, fbr = 1.0;
    int K = 0, Kb = 0, hb0 = 0, tpos = 0;
    const std::uint8_t* envQ = nullptr;   // K x J
    const std::uint8_t* phQ = nullptr;    // K
    const std::int8_t* devQ = nullptr;    // K
    const std::int8_t* pitchQ = nullptr;  // P
    std::vector<std::uint8_t> envBQ, phBQ;  // Kb x J, Kb (expanded from the stored rows)
    std::vector<double> teq;              // attack EQ gains per band, empty for none
};

struct Kit
{
    double fs = 44100.0, tEnd = 6.0, gamma = 2.2, envStep = 0.4, centStep = 0.5, devDiv = 256.0, merge = 0.12;
    int J = 24, P = 12, block = 64;
    std::vector<double> teqFc;
    double teqQ = 1.41;
    std::vector<int> midi;                // the analysed pitches
    int numLayers = 4;
    std::vector<float> tstream;           // every note's transient, int16 values as float
    std::vector<double> grid, pgrid;      // envelope and pitch-curve time points
    std::vector<Note> notes;
    std::vector<std::vector<const Note*>> at;   // [pitch index][layer index]
    std::vector<std::uint8_t> data;       // the whole file; the notes point into it

    int maxTransientSamples = 0;

    // Null if the bytes are not a BAS1 kit this reads.
    static std::unique_ptr<Kit> parse (const void* bytes, std::size_t size);

    double lowestMidi() const   { return midi.empty() ? 0.0 : midi.front(); }
    double highestMidi() const  { return midi.empty() ? 0.0 : midi.back(); }
};

inline double midiHz (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }

// One note's worth of parameters, interpolated from the analysed notes around
// it. Pitch is a (fractional) MIDI note; velocity 0..1 sweeps the four layers.
struct Spec
{
    double midi = 0.0, f0 = 0.0, B = 0.0, top = 0.0, fbr = 1.0, ratio = 1.0, transGain = 0.0;
    int K = 0, hb0 = 0, Kb = 0;
    std::vector<double> lnEnv, dev, phase, pitchCents, lnEnvB, phaseB;
    const Note* prim = nullptr;

    void reserve (const Kit&);
};

void makeSpec (const Kit&, double midi, double velocity, double tuneSemitones, Spec& out);

// A pitch move from `from` to `ratio` (frequency ratios) over `dur` seconds
// from `at`, the finger travelling smoothly. Fretted, the pitch is the fret
// under the finger: it climbs in semitone steps, each step a quick slur
// rather than a jump, the way a string drawn along a fretted neck sounds.
// Unfretted it glides, as on a fretless.
struct Bend
{
    double at = 0.0, dur = 0.1, from = 1.0, ratio = 1.0;
    bool fretted = false;

    // Where the finger is, u = 0..1 of the way, as semitones from `from`.
    static double semitonesAt (double u, double totalSemitones, bool fretted);
};

// Everything at its default plays the analysed note as it was recorded.
struct Options
{
    bool harmonics = true, transient = true;
    double decay = 1.0;      // tail length factor (after the attack)
    double bright = 0.0;     // tilt, dB per octave about 200Hz
    double body = 0.0;       // dB on the lowest partials, loudness-compensated
    double pick = 0.0;       // plucking point as a fraction of the string; 0 = as recorded
    double buzz = 0.0;       // fret buzz, dB; -60 or below drops it
    double mute = 0.0;       // palm mute 0..1
    double attack = 0.0;     // transient, dB
    double attackEq = 0.5;   // how far to even out the transients' colour, 0..1
    double gain = 0.0;       // dB
    double dur = 12.0;       // longest the note may be, seconds

    // Legato: start this many seconds into the note (phase and envelope
    // fast-forwarded, so the waveform matches a voice started at 0), and
    // pitch bends. Later bends win where they overlap.
    double start = 0.0;
    static constexpr int maxBends = 4;
    std::array<Bend, maxBends> bends {};
    int numBends = 0;

    void addBend (const Bend&);
};

class Voice
{
public:
    void reserve (const Kit&);

    // The spec is copied: the voice owns what it plays.
    void start (const Kit&, const Spec&, double sampleRate, const Options&);

    // Overwrites out[0..count); returns how many samples were written (fewer
    // only at the end).
    int render (float* out, int count);

    bool isDone() const       { return outPos >= n; }
    int getLength() const     { return n; }
    int getPosition() const   { return outPos; }       // samples since the note began, including `start`
    double getSampleRate() const { return fs; }

    // The partials' RMS level at time t (seconds since the note began).
    double levelAt (double t) const;
    double bendAt (double t) const;

    // The partials, for drawing: string partials first, then the buzz.
    int getNumComponents() const              { return K; }
    int getNumStringPartials() const          { return KS; }
    double getComponentFrequency (int c) const { return fk[(size_t) c]; }
    bool isComponentAudible (int c) const     { return on[(size_t) c] != 0; }

    // A partial's amplitude at time t, in dB (very low when silent).
    double getComponentLevelDb (int c, double t) const;

    const Spec& getSpec() const       { return spec; }
    const Options& getOptions() const { return opts; }

private:
    double modelTime (double t) const   { return t <= ta || decay == 1.0 ? t : ta + (t - ta) / decay; }
    void envAt (double t, double* dst);
    double pitchAt (double t);
    int blockLen (int p) const;
    void genBlock();

    const Kit* kit = nullptr;
    Spec spec;
    Options opts;
    double fs = 44100.0, decay = 1.0, ta = 0.0, step = 1.0, aT = 0.0, U = 0.0, bodyComp = 0.0;
    int J = 24, BLOCK = 64, BIG = 256, KS = 0, KB = 0, K = 0, naS = 0, na = 0, n = 0, fo = 0;
    int jE = 0, jP = 0, pos = 0, blen = 0, rd = 0, outPos = 0;

    std::vector<double> fk, gk, mk, phi, lnPrev, lnNext, lnEnv, R0, R1, C1, C2, blk;
    std::vector<std::uint8_t> on;
    std::vector<float> trans;   // this note's transient, EQ'd if asked
};

} // namespace carve::plugins::bass
