#pragma once

#include <array>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins
{

// Overtop: three-band upward *and* downward compression, the trick behind the
// dense, bright, in-your-face sound that a plain compressor cannot get to.
//
// A normal compressor only pushes loud things down. This pushes quiet things
// up as well, in each band separately, which is what fills every gap in a
// sound: the tail of a snare comes up to meet its transient, the air above a
// pad stops dipping between notes, and a whole mix arrives sounding like it
// has been leaned on for an hour. Overdone it is a well-known kind of ugly,
// which is exactly why Depth is the knob nearest the front: at 25% it
// thickens, at 100% it is an effect.
//
// The shape, per band:
//
//        output dB
//            |          . downward, above the threshold (20:1 by default)
//            |       ..'
//            |    ..'
//            | ..'   <- upward, below it (4:1)
//            +----------------- input dB
//                 ^ threshold
//
// Both slopes meet at the threshold through a soft knee, so a signal sitting
// right at it does not chatter between the two.
//
// The crossover is Linkwitz-Riley 4th order, and the bands are put back
// together the way they have to be if the dry/wet control is going to mean
// anything: LR4 sums to an allpass rather than to the original signal, so the
// dry side of the mix is the *unprocessed sum of the same bands* rather than
// the input. Both halves then carry the same phase and the mix stays a
// crossfade instead of a comb filter. The high band goes through the low
// crossover's allpass on its way past, which is what keeps three bands aligned
// rather than two.
//
// Built-in like the rest of ours, so its settings are plain properties in the
// .carve and a song opens without a plugin scan (see EngineSetup.h).
class OvertopPlugin : public te::Plugin
{
public:
    explicit OvertopPlugin (te::PluginCreationInfo);
    ~OvertopPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Overtop"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Overtop"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Overtop"; }
    juce::String getSelectableDescription() override  { return TRANS ("Overtop"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    juce::CachedValue<float> depthValue, lowMidHzValue, midHighHzValue,
                             thresholdDbValue, downwardRatioValue, upwardRatioValue,
                             attackMsValue, releaseMsValue,
                             lowGainDbValue, midGainDbValue, highGainDbValue,
                             maxUpwardDbValue, outputDbValue;

    te::AutomatableParameter::Ptr depth, lowMidHz, midHighHz,
                                  thresholdDb, downwardRatio, upwardRatio,
                                  attackMs, releaseMs,
                                  lowGainDb, midGainDb, highGainDb,
                                  maxUpwardDb, outputDb;

private:
    static constexpr int maxChannels = 2;
    static constexpr int numBands = 3;

    // Transposed direct form II: one multiply-add per coefficient and no state
    // shuffling, and it keeps its poise at the low corner frequencies the
    // bottom crossover sits at.
    struct BiquadCoefficients
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };

    struct BiquadState
    {
        void reset() noexcept  { z1 = z2 = 0.0; }

        double process (const BiquadCoefficients& c, double x) noexcept
        {
            const auto y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }

        double z1 = 0.0, z2 = 0.0;
    };

    // Linkwitz-Riley 4th order is two identical Butterworth sections in
    // series, which is the whole reason it sums flat: each half of a crossover
    // is the square of a filter whose -3dB points meet, so the pair adds up to
    // -6dB in phase at the corner instead of a bump or a dip.
    struct LinkwitzRiley
    {
        void reset() noexcept  { first.reset(); second.reset(); }

        double process (const BiquadCoefficients& c, double x) noexcept
        {
            return second.process (c, first.process (c, x));
        }

        BiquadState first, second;
    };

    // Everything one channel needs to be split into three: the two crossovers,
    // plus the pair that puts the high band through the low crossover's
    // allpass so it arrives in step with the other two.
    struct Splitter
    {
        void reset() noexcept
        {
            for (auto* f : { &lowOfSplit, &highOfSplit, &lowOfBass, &highOfBass, &alignLow, &alignHigh })
                f->reset();
        }

        LinkwitzRiley lowOfSplit, highOfSplit;   // at the mid/high corner
        LinkwitzRiley lowOfBass, highOfBass;     // the low/mid corner, on what is left
        LinkwitzRiley alignLow, alignHigh;       // the same corner, summed, as an allpass
    };

    // The state a band's dynamics carry between blocks. The envelope moves
    // every sample so a transient is caught; the gain it implies is worked out
    // at a slower rate and slewed, which is what keeps the log and the
    // exponential off the per-sample path and the gain free of steps.
    struct BandDynamics
    {
        void reset() noexcept  { envelope = fastEnvelope = 0.0f; gain = targetGain = 1.0f; }

        float envelope = 0.0f;

        // A second detector that rises the instant the band does, used for
        // one thing: to take the boost away. Upward compression decides how
        // far to lift a quiet signal, and the attack-shaped envelope above is
        // still reporting "quiet" for a few milliseconds after a transient has
        // arrived -- which is how an upward compressor ends up amplifying the
        // very thing it should be getting out of the way of. Downward
        // compression still works from the slow one, so Attack keeps meaning
        // what the panel says it means.
        float fastEnvelope = 0.0f;

        float gain = 1.0f, targetGain = 1.0f;
    };

    // Recomputed once a block from the parameters.
    struct BlockSettings
    {
        BiquadCoefficients lowPassBass, highPassBass, lowPassSplit, highPassSplit;
        float attackCoefficient = 1.0f, releaseCoefficient = 1.0f, gainSlew = 1.0f;
        float thresholdDb = -30.0f, downwardSlope = 0.05f, upwardSlope = 0.25f;
        float maxUpwardDb = 30.0f;
        std::array<float, numBands> bandGain { 1.0f, 1.0f, 1.0f };
        float depth = 0.25f, outputGain = 1.0f;
    };

    // One Butterworth section; two in series make one half of an LR4
    // crossover. Members rather than free functions only because the
    // coefficient type is one.
    static BiquadCoefficients butterworthLowPass (double hz, double sampleRate);
    static BiquadCoefficients butterworthHighPass (double hz, double sampleRate);

    BlockSettings readSettings();

    // The two-sided curve: what to do with a band sitting at this level.
    // `instantLevelDb` is the same band as it is *right now*, which is what
    // decides whether a boost is still allowed.
    float gainForLevel (float levelDb, float instantLevelDb, const BlockSettings&) const noexcept;

    // The crossover corners follow the knobs rather than jumping to them: a
    // biquad handed brand new coefficients mid-note clicks, and these are the
    // only coefficients here that a user can move.
    double smoothedLowMidHz = 90.0, smoothedMidHighHz = 2500.0;

    double currentSampleRate = 44100.0;

    Splitter splitters[maxChannels];
    BandDynamics dynamics[numBands];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OvertopPlugin)
};

} // namespace carve::plugins
