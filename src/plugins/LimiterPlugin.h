#pragma once

#include <atomic>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "GainReduction.h"

namespace te = tracktion;

namespace carve::plugins
{

// A brickwall limiter, and the reason every new song has one on its master.
//
// Summing a handful of generators past 0dBFS is the easiest mistake to make in
// a DAW and the least obvious one to hear: the render clips, and what you get
// back is a crackle you then go hunting for in the parts. This sits at the end
// of the master chain and makes that impossible without taking anything away
// from a mix that was never near the ceiling.
//
// It looks ahead. The signal is delayed by a couple of milliseconds while the
// gain is worked out from what is *about* to arrive, so the reduction is
// already in place when the peak gets there -- which is the difference between
// a limiter and a compressor with a fast attack, and the difference between
// catching a transient and squashing everything around it. The gain is the
// running minimum over that window (a monotonic deque, so it costs a push and
// a pop per sample however long the window is), smoothed on the way down and
// released slowly on the way up, with a hard clip at the ceiling behind it all
// as the guarantee.
//
// Stereo-linked: both channels take the same gain, or a loud left would pull
// the image over every time it peaked.
//
// The latency it adds is reported, so the engine lines the rest of the mix up
// with it rather than letting the master drift a couple of milliseconds late.
class LimiterPlugin : public te::Plugin,
                      public GainReductionSource
{
public:
    explicit LimiterPlugin (te::PluginCreationInfo);
    ~LimiterPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Limiter"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Limiter"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Limit"; }
    juce::String getSelectableDescription() override  { return TRANS ("Limiter"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer (const te::PluginRenderContext&) override;
    double getLatencySeconds() override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    float getGainReductionDb() const noexcept override  { return gainReductionDb.load (std::memory_order_relaxed); }

    juce::CachedValue<float> inputDbValue, ceilingDbValue, releaseMsValue;
    te::AutomatableParameter::Ptr inputDb, ceilingDb, releaseMs;

private:
    // The running minimum of the target gain over the lookahead window. A
    // plain scan would be O(window) a sample; keeping the deque monotonic
    // makes it O(1) amortised, which is what lets the window be long enough
    // to matter.
    struct MinWindow
    {
        void resize (int windowLength);
        void reset();
        // Pushes the newest value and returns the minimum over the window.
        float push (float value) noexcept;

        std::vector<float> values;   // ring of candidates, oldest first
        std::vector<int> indices;    // their positions in the sample count
        int head = 0, tail = 0;      // deque bounds into the rings above
        int position = 0;            // samples pushed so far
        int length = 1;
    };

    static constexpr int maxChannels = 2;

    // Two milliseconds: long enough to get out of the way of a snare
    // transient, short enough that the latency it costs the master is
    // inaudible and the engine's compensation is trivial.
    static constexpr double lookaheadSeconds = 0.002;

    // How fast the gain may fall once the window has decided it should. A
    // fraction of the lookahead, so it arrives before the peak does and still
    // reads as a slope rather than a step.
    static constexpr double attackSeconds = 0.0004;

    std::atomic<float> gainReductionDb { 0.0f };

    double currentSampleRate = 44100.0;
    int lookaheadSamples = 1;
    float attackCoefficient = 1.0f, releaseCoefficient = 1.0f;
    float currentGain = 1.0f;

    juce::AudioBuffer<float> delayBuffer;
    int delayWritePos = 0;
    MinWindow minWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterPlugin)
};

} // namespace carve::plugins
