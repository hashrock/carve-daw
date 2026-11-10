#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins
{

// The one effect on the wish list that tracktion doesn't already ship.
//
// A drive stage into a soft-clipping waveshaper, with a tone control and a
// dry/wet mix. Deliberately small: the point is to have a distortion that
// travels with the song rather than to compete with a dedicated plugin. Being
// internal, its settings live in the .carve as plain properties and a song
// opens on a machine that has never scanned a plugin.
class DistortionPlugin : public te::Plugin
{
public:
    explicit DistortionPlugin (te::PluginCreationInfo);
    ~DistortionPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Distortion"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Distortion"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Dist"; }
    juce::String getSelectableDescription() override  { return TRANS ("Distortion"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    // The output a freshly inserted one starts at. The shaper is normalised so
    // full scale comes out at full scale, which leaves a track at an ordinary
    // level (-18dBFS RMS) +11.0dB louder at the default drive the moment it is
    // inserted -- a jump nobody asked for, and one that makes any A/B with the
    // bypass a comparison of loudness. This brings that case back to about
    // unity. Applied on insertion only (EditSync), not as the property's
    // default: a saved song that never moved the knob relies on 0dB.
    static constexpr float insertedOutputDb = -11.0f;

    juce::CachedValue<float> driveDbValue, toneValue, mixValue, outputDbValue;
    te::AutomatableParameter::Ptr driveDb, tone, mix, outputDb;

private:
    // One-pole low pass, so the tone control can take the fizz off the top of
    // the clipped signal without pulling in a whole filter.
    struct OnePole
    {
        void setCoefficient (float c) noexcept  { a = c; }
        void reset() noexcept                   { z = 0.0f; }

        float process (float x) noexcept
        {
            z += a * (x - z);
            return z;
        }

        float a = 1.0f, z = 0.0f;
    };

    OnePole toneFilter[2];
    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DistortionPlugin)
};

} // namespace carve::plugins
