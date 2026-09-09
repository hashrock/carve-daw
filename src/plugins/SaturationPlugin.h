#pragma once

#include <atomic>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins
{

// The gentle sibling of DistortionPlugin: an asymmetric tanh waveshaper for
// warming a sound rather than wrecking it.
//
// Asymmetric because the even harmonics are what read as "tube" -- a
// symmetric clip only ever adds odd ones. The shaper runs at 2x so the
// harmonics it adds above half the sample rate fold back an octave higher
// than they would otherwise, where the halfband filter removes most of them;
// at the drive levels this plugin allows that is enough, and the polyphase
// IIR halfband is cheap (a handful of allpass sections per sample).
//
// Like the distortion it is a built-in type: its settings are plain
// properties in the .carve and a song opens without any plugin scan.
class SaturationPlugin : public te::Plugin
{
public:
    explicit SaturationPlugin (te::PluginCreationInfo);
    ~SaturationPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Saturation"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Saturation"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Sat"; }
    juce::String getSelectableDescription() override  { return TRANS ("Saturation"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer (const te::PluginRenderContext&) override;
    double getLatencySeconds() override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    juce::CachedValue<float> driveDbValue, toneValue, mixValue, outputDbValue;
    te::AutomatableParameter::Ptr driveDb, tone, mix, outputDb;

private:
    // The tone control is a tilt: one crossover, lows down as highs go up and
    // vice versa. One pole is enough for a broad tilt, and being a shelf pair
    // rather than a low pass it never dulls the sound at its centre setting.
    struct Tilt
    {
        void setCoefficient (float c) noexcept  { a = c; }
        void reset() noexcept                   { z = 0.0f; }

        float process (float x, float lowGain, float highGain) noexcept
        {
            z += a * (x - z);
            return z * lowGain + (x - z) * highGain;
        }

        float a = 1.0f, z = 0.0f;
    };

    // An asymmetric shaper leaves a DC component behind; this takes it out
    // before it reaches anything downstream that dislikes it (the compressor's
    // detector, for one).
    struct DCBlocker
    {
        void setCoefficient (float c) noexcept  { r = c; }
        void reset() noexcept                   { x1 = y1 = 0.0f; }

        float process (float x) noexcept
        {
            const auto y = x - x1 + r * y1;
            x1 = x;
            y1 = y;
            return y;
        }

        float r = 0.995f, x1 = 0.0f, y1 = 0.0f;
    };

    static constexpr int maxChannels = 2;

    juce::dsp::Oversampling<float> oversampling;
    juce::AudioBuffer<float> dryBuffer;
    Tilt tilt[maxChannels];
    DCBlocker dcBlocker[maxChannels];
    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SaturationPlugin)
};

} // namespace carve::plugins
