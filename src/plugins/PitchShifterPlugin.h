#pragma once

#include <memory>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins
{

// Moves a sound up or down by semitones without changing how long it lasts.
//
// tracktion has a Pitch Shifter of its own, and this replaces it. That one is
// a wrapper round the engine's TimeStretcher, whose only backend that comes
// with the engine is SoundTouch -- LGPL, which a statically linked App Store
// binary cannot honour (nobody can relink it). So the stretcher here is
// Signalsmith Stretch, a phase vocoder under the MIT licence, and no
// tracktion time-stretcher is compiled in at all.
//
// It keeps the engine plugin's parameter id and state property, so a song
// saved with the old one opens with its setting intact: EditSync maps the old
// type name ("pitchShifter") onto this plugin.
//
// A phase vocoder works in overlapping blocks, so it is late by a block or
// so (about 0.15 s). That latency is fixed whatever the shift, and it is
// reported, so the engine lines the rest of the mix up with it.
class PitchShifterPlugin : public te::Plugin
{
public:
    explicit PitchShifterPlugin (te::PluginCreationInfo);
    ~PitchShifterPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Pitch Shifter"); }
    static const char* xmlTypeName;

    // What tracktion's own pitch shifter is called in a song saved before
    // this one existed.
    static const char* legacyXmlTypeName;

    juce::String getName() const override             { return "Pitch Shifter"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Pitch"; }
    juce::String getSelectableDescription() override  { return TRANS ("Pitch Shifter"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer (const te::PluginRenderContext&) override;
    double getLatencySeconds() override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    static constexpr float maxSemitones = 24.0f;

    juce::CachedValue<float> semitonesValue;
    te::AutomatableParameter::Ptr semitones;

private:
    // The stretcher is a large template; keeping it behind a pointer keeps
    // its headers out of everything that includes this one.
    struct Stretcher;
    std::unique_ptr<Stretcher> stretcher;

    double latencySeconds = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchShifterPlugin)
};

} // namespace carve::plugins
