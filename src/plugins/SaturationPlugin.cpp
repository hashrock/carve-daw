#include "SaturationPlugin.h"

namespace carve::plugins
{

namespace
{
    const juce::Identifier driveDbId ("driveDb");
    const juce::Identifier toneId ("tone");
    const juce::Identifier mixId ("mix");
    const juce::Identifier outputDbId ("outputDb");

    // How far off-centre the tanh sits. Enough for the even harmonics to be
    // audible at moderate drive, not so much that the waveform visibly leans.
    constexpr float shaperBias = 0.25f;

    // Where the tilt pivots, and how much it can lean either way.
    constexpr double tiltCrossoverHz = 1000.0;
    constexpr float tiltRangeDb = 6.0f;

    constexpr double dcBlockerHz = 10.0;

    // The tanh is shifted by the bias and re-centred so silence stays at zero,
    // then normalised the same way the distortion is: unity in is unity out
    // once drive is past 0dB, so turning drive up gets thicker rather than
    // louder and the mix control blends rather than fades.
    struct Shaper
    {
        explicit Shaper (float driveGain) noexcept
            : gain (driveGain),
              offset (std::tanh (shaperBias)),
              scale (1.0f / (std::tanh (juce::jmax (1.0f, driveGain) + shaperBias) - std::tanh (shaperBias)))
        {
        }

        float operator() (float x) const noexcept
        {
            return (std::tanh (x * gain + shaperBias) - offset) * scale;
        }

        float gain, offset, scale;
    };
} // namespace

const char* SaturationPlugin::xmlTypeName = "carveSaturation";

SaturationPlugin::SaturationPlugin (te::PluginCreationInfo info)
    : te::Plugin (info),
      // 2x, polyphase IIR halfband: the cheap option, and its latency is a
      // couple of samples rather than the FIR's tens.
      oversampling (maxChannels, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, false)
{
    auto um = getUndoManager();

    driveDbValue.referTo (state, driveDbId, um, 8.0f);
    toneValue.referTo (state, toneId, um, 0.0f);
    mixValue.referTo (state, mixId, um, 1.0f);
    outputDbValue.referTo (state, outputDbId, um, 0.0f);

    driveDb = addParam ("drive", TRANS ("Drive"), { 0.0f, 36.0f },
                        [] (float v)               { return juce::String (juce::roundToInt (v)) + " dB"; },
                        [] (const juce::String& s) { return s.getFloatValue(); });

    tone = addParam ("tone", TRANS ("Tone"), { -1.0f, 1.0f },
                     [] (float v)
                     {
                         const auto percent = juce::roundToInt (v * 100.0f);
                         return (percent > 0 ? "+" : "") + juce::String (percent) + "%";
                     },
                     [] (const juce::String& s) { return s.getFloatValue() * 0.01f; });

    mix = addParam ("mix", TRANS ("Mix"), { 0.0f, 1.0f },
                    [] (float v)               { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; },
                    [] (const juce::String& s) { return s.getFloatValue() * 0.01f; });

    outputDb = addParam ("output", TRANS ("Output"), { -24.0f, 12.0f },
                         [] (float v)               { return juce::String (v, 1) + " dB"; },
                         [] (const juce::String& s) { return s.getFloatValue(); });

    driveDb->attachToCurrentValue (driveDbValue);
    tone->attachToCurrentValue (toneValue);
    mix->attachToCurrentValue (mixValue);
    outputDb->attachToCurrentValue (outputDbValue);
}

SaturationPlugin::~SaturationPlugin()
{
    notifyListenersOfDeletion();

    driveDb->detachFromCurrentValue();
    tone->detachFromCurrentValue();
    mix->detachFromCurrentValue();
    outputDb->detachFromCurrentValue();
}

void SaturationPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    currentSampleRate = info.sampleRate;

    // The only allocations, made here so applyToBuffer never has to. A block
    // longer than this is processed in pieces rather than trusted not to
    // happen.
    const auto capacity = juce::jmax (1, info.blockSizeSamples);
    oversampling.initProcessing ((size_t) capacity);
    oversampling.reset();
    dryBuffer.setSize (maxChannels, capacity);

    for (auto& t : tilt)
        t.reset();

    for (auto& blocker : dcBlocker)
        blocker.reset();
}

void SaturationPlugin::deinitialise()
{
}

double SaturationPlugin::getLatencySeconds()
{
    return oversampling.getLatencyInSamples() / currentSampleRate;
}

void SaturationPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    const auto numChannels = juce::jmin (maxChannels, fc.destBuffer->getNumChannels());
    const auto capacity = dryBuffer.getNumSamples();

    if (numChannels == 0 || capacity == 0)
        return;

    const Shaper shape (te::dbToGain (driveDb->getCurrentValue()));
    const auto tiltAmount = tone->getCurrentValue();
    const auto lowGain = te::dbToGain (-tiltRangeDb * tiltAmount);
    const auto highGain = te::dbToGain (tiltRangeDb * tiltAmount);
    const auto wet = mix->getCurrentValue();
    const auto outGain = te::dbToGain (outputDb->getCurrentValue());

    const auto tiltCoefficient = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi
                                                          * tiltCrossoverHz / currentSampleRate));
    const auto dcCoefficient = (float) (1.0 - 2.0 * juce::MathConstants<double>::pi
                                              * dcBlockerHz / currentSampleRate);

    for (int channel = 0; channel < numChannels; ++channel)
    {
        tilt[channel].setCoefficient (tiltCoefficient);
        dcBlocker[channel].setCoefficient (dcCoefficient);
    }

    for (int start = 0; start < fc.bufferNumSamples; start += capacity)
    {
        const auto numSamples = juce::jmin (capacity, fc.bufferNumSamples - start);
        const auto bufferStart = fc.bufferStartSample + start;

        for (int channel = 0; channel < numChannels; ++channel)
            dryBuffer.copyFrom (channel, 0, *fc.destBuffer, channel, bufferStart, numSamples);

        // The wet path is shaped in place at 2x, then brought back down. The
        // dry copy is not delayed to match: the halfband's latency is under
        // two samples, so the comb it makes against the dry sits above 10kHz
        // and only at a partial mix -- not worth a delay line.
        auto wetBlock = juce::dsp::AudioBlock<float> (*fc.destBuffer)
                            .getSubBlock ((size_t) bufferStart, (size_t) numSamples)
                            .getSubsetChannelBlock (0, (size_t) numChannels);

        auto upsampled = oversampling.processSamplesUp (wetBlock);

        for (size_t channel = 0; channel < upsampled.getNumChannels(); ++channel)
        {
            auto* samples = upsampled.getChannelPointer (channel);

            for (size_t i = 0; i < upsampled.getNumSamples(); ++i)
                samples[i] = shape (samples[i]);
        }

        oversampling.processSamplesDown (wetBlock);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto* samples = fc.destBuffer->getWritePointer (channel, bufferStart);
            const auto* dry = dryBuffer.getReadPointer (channel);
            auto& t = tilt[channel];
            auto& blocker = dcBlocker[channel];

            for (int i = 0; i < numSamples; ++i)
            {
                const auto shaped = blocker.process (t.process (samples[i], lowGain, highGain));
                samples[i] = (dry[i] * (1.0f - wet) + shaped * wet) * outGain;
            }
        }
    }
}

void SaturationPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, driveDbValue, toneValue, mixValue, outputDbValue);

    // The parameters cache their value alongside the tree and processing reads
    // the parameter, so a property write alone leaves the sound at the
    // construction defaults.
    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

} // namespace carve::plugins
