#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace carve::plugins::bassmodel
{

// A physically modelled electric bass: no recordings, four strings computed a
// sample at a time, then the magnetic pickups and an amp. A C++ port of the
// web-bass-model prototype's bass-core.js (an AudioWorklet); the comments
// there and here describe the same model.
//
//   right hand (finger / pick / thumb / pop)
//     -> four strings (digital waveguides, one per polarisation: vertical,
//        towards the frets, and horizontal; stiffness as a cascade of
//        allpasses, frequency-dependent loss as a one-pole lowpass; the
//        vertical one collides with the three frets past the stopped one --
//        the buzz)
//     -> two magnetic pickups, Jazz Bass style (where they sit decides which
//        partials they miss; flux ~ 1/(distance - displacement)^2, so a
//        pickup close to the strings growls), the coil's resonance as a tone
//        control -> DI, or an amp's drive and cabinet
//
// The fretting hand is modelled too: which string and fret a note goes to
// (near where the hand was, avoiding strings still ringing), hammer-ons and
// pull-offs that change the string's length without plucking it, and slides
// that step across the frets on the way (or glide, fretless).
//
// Displacement is in arbitrary units (an ordinary finger pluck is about 1).
// Realtime-safe once prepared: prepare() allocates everything, and the
// expensive half of every string and fret's design (the dispersion fit) is
// done there; nothing after it allocates or locks.

constexpr int numStrings = 4;
constexpr int numFrets = 21;
constexpr std::array<int, numStrings> tuning { 28, 33, 38, 43 };   // E1 A1 D2 G2
constexpr int lowestNote = 28, highestNote = 43 + numFrets;          // E1 .. E4
constexpr double scaleLength = 0.864;                                  // 34" [m]
constexpr std::array<double, 2> pickupPositions { 0.235, 0.115 };      // from the bridge, as a fraction of the scale: neck, bridge

enum class Style { finger, pick, thumb, pop, slapAndPop };   // slapAndPop: thumb on E and A, popped on D and G
enum class Legato { retrigger, hammer, slide };              // what a note played while another is held does

struct Options
{
    int style = 0;           // Style
    double pluck = 0.2;      // where the string is plucked, from the bridge as a fraction of the scale
    double pickup = 0.5;     // 0 neck .. 1 bridge pickup
    double height = 0.5;     // pickup closeness 0 far .. 1 close (louder, growls)
    double tone = 0.7;       // the coil's resonance / tone knob
    double action = 0.5;     // string height 0 low (buzzes) .. 1 high
    double sustain = 1.0;    // decay time, as a factor
    double bright = 0.5;     // how new the strings are (how long the upper partials ring)
    double mute = 0.0;       // palm mute at the bridge
    double slide = 0.09;     // seconds a slide takes
    bool fretless = false;   // slides glide rather than stepping across the frets
    int legato = 1;          // Legato
    double drive = 0.2;      // amp drive
    double amp = 0.4;        // 0 DI .. 1 amp and cabinet
    double volume = 0.8;
};

// The prototype's sounds; their volumes level the demo phrases they go with.
constexpr int numFactoryPresets = 9;
const char* getFactoryPresetName (int index);
Options getFactoryPreset (int index);

double midiToHz (double midi);

// The allpass cascade that gives one stopped string its stretched partials.
struct Dispersion
{
    int stages = 0;
    double a = 0.0;
};

Dispersion designDispersion (double f1, double inharmonicity, double sampleRate, double lowpass);

// One string at one fret: the loop's loss (g, b), its dispersion, and the
// one-way delay of each rail in samples.
struct FretDesign
{
    double f1 = 0.0, g = 0.0, b = 0.0, a = 0.0, rail = 0.0;
    int stages = 0;
};

FretDesign designFret (int string, int fret, double sampleRate, const Options&, const Dispersion&);
double inharmonicityOf (int string, int fret);

//==============================================================================
class Bass
{
public:
    Bass();
    ~Bass();

    // Allocates, and fits every string and fret's dispersion. Not realtime.
    void prepare (double sampleRate, std::uint32_t seed = 12345);

    // Realtime, once prepared.
    void setOptions (const Options&);
    const Options& getOptions() const  { return options; }

    // velocity 0..1. legato: if a held note's string can reach this one, move
    // that string's finger instead of plucking (up a hammer-on, down a
    // pull-off); slide: slide there.
    void noteOn (int midi, double velocity, bool legato = false, bool slide = false);
    void noteOff (int midi);
    void allNotesOff();   // lifts every finger (the strings die away in a fraction of a second)

    // Writes n samples (mono) over out.
    void process (float* out, int n);

    // Which string and fret a note would go to now (-1 string when out of reach).
    struct Place { int string = -1, fret = 0; };
    Place place (int midi) const;

    // For the editor: each string's stopped fret, the note held on it (-1 for
    // none) and its displacement from the bridge to the fret, at 32 points.
    struct StringView
    {
        int fret = 0, held = -1;
        std::array<float, 32> shape {};
    };
    StringView view (int string) const;
    int getSoundingCount() const;

    // Where the hand is (the last stopped fret). Settable for tests.
    int lastFret = 5;

private:
    struct Str;

    double rand();
    void setTone();
    void setCab();
    void redesignStrings();
    const Dispersion& dispersionFor (int string, int fret) const;

    bool prepared = false;
    double fs = 48000.0;
    Options options;
    std::uint32_t seedState = 12345;

    std::vector<Dispersion> dispersions;          // numStrings * (numFrets + 1)
    std::array<std::unique_ptr<Str>, numStrings> strings;

    // Held notes, in the order they were played: (midi, string).
    std::array<std::pair<int, int>, 8> held {};
    int numHeld = 0;
    int heldString (int midi) const;
    void dropHeld (int midi);

    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0; double run (double); };
    Biquad toneFilter;
    std::array<Biquad, 3> cab;
    double dcX = 0.0, dcY = 0.0;
};

} // namespace carve::plugins::bassmodel
