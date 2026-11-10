#include "BitcrusherPlugin.h"

namespace carve::plugins
{

namespace
{
    const juce::Identifier bitsId ("bits");
    const juce::Identifier rateHzId ("rateHz");
    const juce::Identifier mixId ("mix");
    const juce::Identifier outputDbId ("outputDb");

    constexpr float minBits = 1.0f, maxBits = 16.0f;
    constexpr float minRateHz = 200.0f, maxRateHz = 48000.0f;

    // Quantise onto a grid of 2^(bits-1) steps either side of zero. A real
    // bit count makes the step size continuous; at one bit the signal is
    // just its sign, a square wave of whatever came in.
    float quantise (float x, float stepsPerUnit) noexcept
    {
        return std::round (juce::jlimit (-1.0f, 1.0f, x) * stepsPerUnit) / stepsPerUnit;
    }
} // namespace

const char* BitcrusherPlugin::xmlTypeName = "carveBitcrusher";

BitcrusherPlugin::BitcrusherPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    auto um = getUndoManager();

    bitsValue.referTo (state, bitsId, um, 8.0f);
    rateHzValue.referTo (state, rateHzId, um, 11025.0f);
    mixValue.referTo (state, mixId, um, 1.0f);
    outputDbValue.referTo (state, outputDbId, um, 0.0f);

    bits = addParam ("bits", TRANS ("Bits"), { minBits, maxBits },
                     [] (float v)               { return juce::String (v, 1) + " bit"; },
                     [] (const juce::String& s) { return s.getFloatValue(); });

    rateHz = addParam ("rate", TRANS ("Rate"), { minRateHz, maxRateHz, 0.0f, 0.3f },
                       [] (float v)
                       {
                           return v < 1000.0f ? juce::String (juce::roundToInt (v)) + " Hz"
                                              : juce::String (v / 1000.0f, 1) + " kHz";
                       },
                       [] (const juce::String& s)
                       {
                           const auto v = s.getFloatValue();
                           return s.containsIgnoreCase ("k") ? v * 1000.0f : v;
                       });

    mix = addParam ("mix", TRANS ("Mix"), { 0.0f, 1.0f },
                    [] (float v)               { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; },
                    [] (const juce::String& s) { return s.getFloatValue() * 0.01f; });

    outputDb = addParam ("output", TRANS ("Output"), { -24.0f, 12.0f },
                         [] (float v)               { return juce::String (v, 1) + " dB"; },
                         [] (const juce::String& s) { return s.getFloatValue(); });

    bits->attachToCurrentValue (bitsValue);
    rateHz->attachToCurrentValue (rateHzValue);
    mix->attachToCurrentValue (mixValue);
    outputDb->attachToCurrentValue (outputDbValue);
}

BitcrusherPlugin::~BitcrusherPlugin()
{
    notifyListenersOfDeletion();

    bits->detachFromCurrentValue();
    rateHz->detachFromCurrentValue();
    mix->detachFromCurrentValue();
    outputDb->detachFromCurrentValue();
}

void BitcrusherPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    currentSampleRate = info.sampleRate;
    reset();
}

void BitcrusherPlugin::deinitialise()
{
}

void BitcrusherPlugin::reset()
{
    // Phase at 1 so the very next sample is taken rather than holding zero.
    holdPhase = 1.0;
    held[0] = held[1] = 0.0f;
}

void BitcrusherPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    const auto bitDepth = juce::jlimit (minBits, maxBits, bits->getCurrentValue());
    const auto stepsPerUnit = std::pow (2.0f, bitDepth - 1.0f);
    const auto wet = mix->getCurrentValue();
    const auto outGain = te::dbToGain (outputDb->getCurrentValue());

    // How far through one held sample each output sample moves. At or above
    // the host rate that is a full step every sample: no decimation at all.
    const auto increment = juce::jlimit (0.0, 1.0, (double) rateHz->getCurrentValue() / currentSampleRate);

    const auto numChannels = juce::jmin (2, fc.destBuffer->getNumChannels());

    float* channels[2] {};

    for (int channel = 0; channel < numChannels; ++channel)
        channels[channel] = fc.destBuffer->getWritePointer (channel, fc.bufferStartSample);

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        holdPhase += increment;
        const auto takeSample = holdPhase >= 1.0;

        if (takeSample)
            holdPhase -= std::floor (holdPhase);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto& sample = channels[channel][i];
            const auto dry = sample;

            if (takeSample)
                held[channel] = quantise (dry, stepsPerUnit);

            sample = (dry * (1.0f - wet) + held[channel] * wet) * outGain;
        }
    }
}

void BitcrusherPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, bitsValue, rateHzValue, mixValue, outputDbValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

} // namespace carve::plugins
