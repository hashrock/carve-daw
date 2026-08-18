#include "DistortionPlugin.h"

namespace orionish::plugins
{

namespace
{
    const juce::Identifier driveDbId ("driveDb");
    const juce::Identifier toneId ("tone");
    const juce::Identifier mixId ("mix");
    const juce::Identifier outputDbId ("outputDb");

    // tanh soft clip. Normalised by the drive gain so turning drive up gets
    // dirtier rather than just louder, which is what makes the mix control
    // usable as a blend rather than a volume.
    float softClip (float x, float driveGain) noexcept
    {
        return std::tanh (x * driveGain) / std::tanh (juce::jmax (1.0f, driveGain));
    }
} // namespace

const char* DistortionPlugin::xmlTypeName = "orionishDistortion";

DistortionPlugin::DistortionPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    auto um = getUndoManager();

    driveDbValue.referTo (state, driveDbId, um, 12.0f);
    toneValue.referTo (state, toneId, um, 0.7f);
    mixValue.referTo (state, mixId, um, 1.0f);
    outputDbValue.referTo (state, outputDbId, um, 0.0f);

    driveDb = addParam ("drive", TRANS ("Drive"), { 0.0f, 48.0f },
                        [] (float v)               { return juce::String (juce::roundToInt (v)) + " dB"; },
                        [] (const juce::String& s) { return s.getFloatValue(); });

    tone = addParam ("tone", TRANS ("Tone"), { 0.0f, 1.0f },
                     [] (float v)               { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; },
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

DistortionPlugin::~DistortionPlugin()
{
    notifyListenersOfDeletion();

    driveDb->detachFromCurrentValue();
    tone->detachFromCurrentValue();
    mix->detachFromCurrentValue();
    outputDb->detachFromCurrentValue();
}

void DistortionPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    currentSampleRate = info.sampleRate;

    for (auto& filter : toneFilter)
        filter.reset();
}

void DistortionPlugin::deinitialise()
{
}

void DistortionPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    const auto driveGain = te::dbToGain (driveDb->getCurrentValue());
    const auto toneAmount = tone->getCurrentValue();
    const auto wet = mix->getCurrentValue();
    const auto outGain = te::dbToGain (outputDb->getCurrentValue());

    // Tone runs the low pass from roughly 800Hz (dark) up to past the top of
    // the band (off), so the control reads as "more tone" turning right.
    const auto cutoff = 800.0 * std::pow (25.0, (double) toneAmount);
    const auto coefficient = (float) juce::jlimit (0.0, 1.0,
                                                   1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi
                                                                        * cutoff / currentSampleRate));

    const auto numChannels = juce::jmin (2, fc.destBuffer->getNumChannels());

    for (int channel = 0; channel < numChannels; ++channel)
    {
        auto* samples = fc.destBuffer->getWritePointer (channel, fc.bufferStartSample);
        auto& filter = toneFilter[channel];
        filter.setCoefficient (coefficient);

        for (int i = 0; i < fc.bufferNumSamples; ++i)
        {
            const auto dry = samples[i];
            const auto distorted = filter.process (softClip (dry, driveGain));

            samples[i] = (dry * (1.0f - wet) + distorted * wet) * outGain;
        }
    }
}

void DistortionPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, driveDbValue, toneValue, mixValue, outputDbValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

} // namespace orionish::plugins
