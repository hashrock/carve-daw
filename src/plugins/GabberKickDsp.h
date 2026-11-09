#pragma once

#include <array>
#include <vector>

#include <juce_core/juce_core.h>

namespace carve::plugins::gabber
{

// The synthesis behind the Gabber Kick instrument: one kick, rendered offline
// from a patch to a buffer.
//
//   sine + click + noise -> (dynamic EQ -> free-curve shaper) x N
//                        -> DC blocker -> FIR decimation -> normalise -> gain
//
// Offline because of what the sound is: a chain of hard non-linearities run at
// four times the output rate and normalised against the peak of the finished
// kick. Peak-normalising needs the whole kick in hand, and a patch does not
// change while it is being hit, so there is nothing to gain from doing this
// per voice on the audio thread.
//
// Free of the engine, so the tests can run it. The same patch at the same rate
// gives the same samples, always -- the noise is seeded, nothing is timed.

struct Point
{
    double x = 0.0, y = 0.0;
    bool operator== (const Point&) const = default;
};

enum class Shape { linear, softClip, hardClip, asymmetric, wavefold, rectifier };
inline constexpr int numShapes = 6;

const char* getShapeName (Shape);

// The curve a shape starts as: nine points the user can then drag anywhere.
// Which shape a stage was given still matters afterwards -- a Wavefold stage
// reflects input beyond the curve's edges, where every other shape squashes it.
std::vector<Point> getShapePoints (Shape);

struct Stage
{
    bool enabled = true;
    double startFreq = 650.0, endFreq = 1400.0;     // the EQ's sweep over the kick's length
    double startGain = 12.0, endGain = -6.0;        // dB
    double q = 1.0;
    double drive = 12.0;                            // dB, into the shaper
    double mix = 0.85;                              // 0..1, dry/wet around the shaper
    double output = -3.0;                           // dB
    Shape shape = Shape::softClip;
    std::vector<Point> points = getShapePoints (Shape::softClip);

    bool operator== (const Stage&) const = default;
};

Stage makeStage (Shape = Shape::softClip);

struct Patch
{
    double base = 55.0, sweep = 180.0;   // Hz: where the pitch settles, and how far above it the hit starts
    double pitchDecay = 0.012;           // seconds
    double attack = 0.0015;              // seconds
    double duration = 0.65;              // seconds
    double click = 0.22, noise = 0.12;   // 0..1
    std::vector<Stage> stages;
    double gain = 0.0;                   // dB
    bool normalize = true;

    bool operator== (const Patch&) const = default;
};

inline constexpr int numFactoryPresets = 4;
const char* getFactoryPresetName (int index);
Patch getFactoryPreset (int index);

struct Render
{
    std::vector<float> samples;
    double peak = 0.0;      // after gain, before protection
    bool limited = false;   // the gain asked for more than -0.1 dBFS, and was turned down
};

Render render (const Patch&, double outputRate);

// What the graphs draw.
double evaluateCurve (const std::vector<Point>&, double x);
double getEqResponseDb (double frequency, double centre, double gainDb, double q);

// Stages as one JSON property of the plugin's tree. One property because a
// preset carries a plugin's properties whole, where it could only carry one
// child tree of each type. Anything missing or malformed reads as its default,
// so a file from a later version still opens.
juce::String stagesToJson (const std::vector<Stage>&);
std::vector<Stage> stagesFromJson (const juce::String&);

} // namespace carve::plugins::gabber
