#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins
{

// Lo-fi by arithmetic: fewer bits and a lower sample rate.
//
// Two stages, each the crudest honest version of itself. Rate is a
// sample-and-hold that only takes a new input sample every so often, with no
// anti-aliasing filter in front of it -- the aliasing *is* the sound. Bits
// rounds what comes out of that onto a coarser grid. Mix blends the result
// back over the dry signal and Output trims the level.
//
// Both controls are continuous: the hold is driven by a fractional phase
// rather than an integer divider, and the step size is 2^(1-bits) for any
// real bit count, so sweeping either under automation glides rather than
// clicking between a handful of settings.
class BitcrusherPlugin : public te::Plugin
{
public:
    explicit BitcrusherPlugin (te::PluginCreationInfo);
    ~BitcrusherPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Bitcrusher"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Bitcrusher"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Crush"; }
    juce::String getSelectableDescription() override  { return TRANS ("Bitcrusher"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    juce::CachedValue<float> bitsValue, rateHzValue, mixValue, outputDbValue;
    te::AutomatableParameter::Ptr bits, rateHz, mix, outputDb;

private:
    // The sample-and-hold. The phase is shared by both channels so a stereo
    // signal is decimated at the same instants on either side.
    double holdPhase = 1.0;
    float held[2] {};
    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BitcrusherPlugin)
};

} // namespace carve::plugins
