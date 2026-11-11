#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "GainReduction.h"
#include "InstrumentFx.h"

namespace te = tracktion;

namespace carve::plugins
{

// The Acoustic Drums' bus compressor as an insert of its own: the same
// soft-knee feed-forward compressor (fx::BusCompressor), stereo-linked, with
// the dry/wet Mix that makes parallel compression one knob instead of a send.
//
// It keeps the names the drum kit's compressor has -- comp* parameters,
// isCompressorOn and the rest -- so the panel that draws the one (see
// CompressorPanel in InstrumentFxPanels.h) draws this too. As an insert it
// is always on; the slot's own bypass switches it off.
class BusCompressorPlugin : public te::Plugin,
                            public GainReductionSource
{
public:
    explicit BusCompressorPlugin (te::PluginCreationInfo);
    ~BusCompressorPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Bus Compressor"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Bus Compressor"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "BusComp"; }
    juce::String getSelectableDescription() override  { return TRANS ("Bus Compressor"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    juce::CachedValue<float> compThresholdValue, compRatioValue, compAttackValue, compReleaseValue,
                             compMakeupValue, compMixValue;
    te::AutomatableParameter::Ptr compThreshold, compRatio, compAttack, compRelease, compMakeup, compMix;

    bool isCompressorOn() const  { return true; }
    void setCompressorOn (bool)  {}
    fx::BusCompressor::Settings getCompressorSettings() const;
    const fx::LevelHistory& getCompressorHistory() const  { return history; }

    float getGainReductionDb() const noexcept override  { return compressor.getReductionDb(); }

private:
    fx::BusCompressor compressor;
    fx::LevelHistory history;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BusCompressorPlugin)
};

} // namespace carve::plugins
