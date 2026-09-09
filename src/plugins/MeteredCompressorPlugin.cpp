#include "MeteredCompressorPlugin.h"

namespace carve::plugins
{

namespace
{
    // Sum of squares over the audio channels (the third, if any, is the
    // sidechain trigger and not part of what the compressor changes).
    double energyOf (const juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
    {
        double energy = 0.0;

        for (int channel = 0; channel < juce::jmin (2, buffer.getNumChannels()); ++channel)
        {
            const auto* samples = buffer.getReadPointer (channel, startSample);

            for (int i = 0; i < numSamples; ++i)
                energy += (double) samples[i] * samples[i];
        }

        return energy;
    }

    // Below this the block is silence as far as the meter is concerned; the
    // ratio of two rounding errors is not a reading.
    constexpr double silenceEnergy = 1.0e-12;
} // namespace

const char* MeteredCompressorPlugin::xmlTypeName = "carveCompressor";

MeteredCompressorPlugin::MeteredCompressorPlugin (te::PluginCreationInfo info)
    : te::CompressorPlugin (info)
{
}

MeteredCompressorPlugin::~MeteredCompressorPlugin()
{
    notifyListenersOfDeletion();
}

void MeteredCompressorPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    const auto inputEnergy = energyOf (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples);

    te::CompressorPlugin::applyToBuffer (fc);

    float reduction = 0.0f;

    if (inputEnergy > silenceEnergy)
    {
        const auto outputEnergy = energyOf (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples);
        const auto gainDb = 10.0 * std::log10 (juce::jmax (silenceEnergy, outputEnergy) / inputEnergy);

        // The output gain is applied inside the same multiply as the
        // reduction, so it has to come back off; the clamp catches the
        // rounding that would otherwise show as a hair of positive gain.
        reduction = (float) juce::jlimit (-100.0, 0.0, gainDb - (double) outputDb.getCurrentValue());
    }

    gainReductionDb.store (reduction, std::memory_order_relaxed);
}

} // namespace carve::plugins
