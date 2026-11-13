#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace carve::plugins::piano
{

// A physically modelled acoustic piano: no samples, every key computed a
// sample at a time from a hammer, its strings, the bridge they share and a
// soundboard. A C++ port of the web-piano prototype's piano-core.js, which
// renders the same thing in an AudioWorklet; the comments there and here
// describe the same model.
//
//   hammer (nonlinear felt: F = K x^p, mass m)
//     -> strings (digital waveguides: stiffness as a cascade of allpasses,
//        frequency-dependent loss as a one-pole lowpass)
//     -> bridge (one junction every string shares: the unison strings'
//        coupling gives the two-stage decay and the beating, and keys ring
//        each other)
//     -> soundboard (low modes as resonators plus a short FDN; the hammer's
//        force itself goes in too, as the knock) -> room (FDN reverb)
//
// SI units throughout (string velocity waves in m/s, hammer force in N). The
// constants are Chaigne & Askenfelt (1994)'s C2 / C4 / C7, interpolated
// across the keyboard.
//
// Realtime-safe once prepared: prepare() allocates every voice and buffer,
// and nothing after it allocates or locks.

constexpr int lowestKey = 21, highestKey = 108;   // A0 .. C8
constexpr int maxStrings = 3;
constexpr int maxAllpasses = 24;

// The knobs. Every one but `detune` takes effect at once; a new detune
// re-tunes the unison strings of the keys struck after it.
struct Options
{
    double volume = 0.8;        // output
    double hardness = 1.0;      // hammer felt stiffness, as a factor on K (brighter when harder)
    double detune = 1.0;        // spread of the unison strings' tuning, as a factor
    double board = 1.0;         // how much soundboard there is
    double brightness = 0.6;    // treble lift (where the mic is, how far the lid is open)
    double knock = 1.0;         // how much of the hammer's blow reaches the soundboard
    double sympathetic = 1.0;   // open strings ringing along while the pedal is down
    double reverb = 0.18;       // the room
};

// The allpass cascade that gives one key its stretched partials.
struct Dispersion
{
    int stages = 0;
    double a = 0.0, error = 0.0;
};

struct StringDesign
{
    int delayOut = 0;            // samples from the strike point back round to the bridge
    double fractionalAllpass = 0.0;
    double hammerWeight = 0.0;
    double gamma = 0.0;          // coupling to the bridge
};

struct KeyDesign
{
    int midi = 0;
    double f1 = 0.0, period = 0.0;
    int strikeDelay = 0;         // d1: samples from the agraffe to the strike point
    Dispersion dispersion;
    double lowpass = 0.0, loopGain = 0.0;
    int numStrings = 0;
    std::array<StringDesign, maxStrings> strings {};
    double impedance = 0.0, hammerMass = 0.0, felt = 0.0, feltExponent = 0.0;
    double dampedGain = 1.0;     // loop gain with the damper down
    double radiation = 1.0;      // how loud the soundboard makes this key
};

double inharmonicity (int midi);

// The expensive half of a key's design, which depends on nothing but the
// key and the sample rate.
Dispersion designDispersion (int midi, double sampleRate);

// The whole design, from that.
KeyDesign designKey (int midi, double sampleRate, double detune, const Dispersion&);

// Every key's dispersion at one sample rate, worked out once per process and
// shared between instances (a few milliseconds the first time).
std::shared_ptr<const std::vector<Dispersion>> getDispersionTable (double sampleRate);

//==============================================================================
class Piano
{
public:
    static constexpr int maxVoices = 48;

    Piano();
    ~Piano();

    // Allocates everything. Not realtime.
    void prepare (double sampleRate);

    // Realtime, once prepared.
    void setOptions (const Options&);
    void noteOn (int midi, double velocity);   // velocity 0..1
    void noteOff (int midi);
    void setPedal (bool down);
    void allNotesOff();                         // silences at once

    // Adds n samples to left and right.
    void process (float* left, float* right, int n);

    int getVoiceCount() const  { return numActive; }

    // True while nothing is sounding at all (not even the room), when
    // process() does nothing.
    bool isSilent() const      { return asleep; }

private:
    struct Voice;
    struct Fdn;
    struct Modes;
    struct Sympathetic;

    Voice* startVoice (int midi);
    void drop (Voice*);
    void steal();
    void recomputeCoupling();
    const KeyDesign& designFor (int midi);

    bool prepared = false;
    double fs = 44100.0;
    Options options;
    double designedDetune = -1.0;

    std::shared_ptr<const std::vector<Dispersion>> dispersions;
    std::array<KeyDesign, 128> designs {};
    std::array<std::uint32_t, 128> designRevision {};
    std::uint32_t revision = 1;

    std::vector<std::unique_ptr<Voice>> voices;
    std::array<Voice*, maxVoices> active {};
    int numActive = 0;
    std::array<Voice*, 128> byKey {};

    std::unique_ptr<Modes> modes;
    std::unique_ptr<Fdn> board, room;
    std::unique_ptr<Sympathetic> sym;

    double vhat = 0.0, couplingSum = 0.0;
    bool sustain = false;
    double knockLp = 0.0, symGain = 0.0;
    double hp1 = 0.0, hp2 = 0.0, hpc = 0.0;
    double rl = 0.0, rr = 0.0, rc = 0.0;
    std::vector<double> preDelay;
    int preDelayPos = 0;
    bool asleep = true;
    int quietSamples = 0;
};

} // namespace carve::plugins::piano
