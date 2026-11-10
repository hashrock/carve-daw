#include "PitchShifterPlugin.h"

#include <array>

#include <signalsmith-stretch/signalsmith-stretch.h>

namespace carve::plugins
{

namespace
{
    // The property tracktion's pitch shifter keeps its setting in. The same
    // one here, so its saved state is this plugin's saved state.
    const juce::Identifier semitonesUpId ("semitonesUp");

    constexpr int maxChannels = 2;

    // The stretcher takes a seed for the noise it uses when asked to stretch
    // time a long way. A pitch shift never gets there, but a fixed seed keeps
    // two renders of a song identical regardless.
    constexpr long randomSeed = 1;

    // What the stretcher writes a stereo result into when the buffer is mono:
    // channel 0 is the buffer, channel 1 is nowhere.
    struct MonoOutput
    {
        struct Channel
        {
            float* data;
            float* sink;
            float& operator[] (int i) const  { return data != nullptr ? data[i] : *sink; }
        };

        Channel operator[] (int channel)  { return { channel == 0 ? left : nullptr, &sink }; }

        float* left;
        float sink = 0.0f;
    };
} // namespace

struct PitchShifterPlugin::Stretcher
{
    signalsmith::stretch::SignalsmithStretch<float> stretch { randomSeed };

    // process() reads its input after it has started writing its output, so
    // it cannot work in place: the block is copied here first.
    juce::AudioBuffer<float> input;
};

const char* PitchShifterPlugin::xmlTypeName = "carvePitchShifter";
const char* PitchShifterPlugin::legacyXmlTypeName = "pitchShifter";

PitchShifterPlugin::PitchShifterPlugin (te::PluginCreationInfo info)
    : te::Plugin (info),
      stretcher (std::make_unique<Stretcher>())
{
    auto um = getUndoManager();

    semitonesValue.referTo (state, semitonesUpId, um, 0.0f);

    // "semitones up" is the engine plugin's parameter id; presets and
    // automation saved against it keep working.
    semitones = addParam ("semitones up", TRANS ("Semitones"), { -maxSemitones, maxSemitones },
                          [] (float v)
                          {
                              if (std::abs (v) < 0.01f)
                                  return juce::String ("Original pitch");

                              return (v > 0.0f ? "+" : "") + juce::String (v, 2) + " st";
                          },
                          [] (const juce::String& s)
                          {
                              return juce::jlimit (-maxSemitones, maxSemitones, s.getFloatValue());
                          });

    semitones->attachToCurrentValue (semitonesValue);
}

PitchShifterPlugin::~PitchShifterPlugin()
{
    notifyListenersOfDeletion();

    semitones->detachFromCurrentValue();
}

void PitchShifterPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    // Every allocation the stretcher makes happens in here; process() only
    // ever resizes within what configure reserved.
    stretcher->stretch.presetDefault (maxChannels, (float) info.sampleRate);
    stretcher->stretch.reset();
    stretcher->input.setSize (maxChannels, juce::jmax (1, info.blockSizeSamples));
    stretcher->input.clear();

    latencySeconds = (stretcher->stretch.inputLatency() + stretcher->stretch.outputLatency()) / info.sampleRate;
}

void PitchShifterPlugin::deinitialise()
{
}

double PitchShifterPlugin::getLatencySeconds()
{
    return latencySeconds;
}

void PitchShifterPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    const auto numChannels = juce::jmin (maxChannels, fc.destBuffer->getNumChannels());
    const auto capacity = stretcher->input.getNumSamples();

    if (numChannels == 0 || capacity == 0)
        return;

    // Not bypassed at zero: the latency has to stay what was reported, and
    // the stretcher at a factor of one is a plain delay near enough.
    stretcher->stretch.setTransposeSemitones (semitones->getCurrentValue());

    for (int start = 0; start < fc.bufferNumSamples; start += capacity)
    {
        const auto numSamples = juce::jmin (capacity, fc.bufferNumSamples - start);
        const auto bufferStart = fc.bufferStartSample + start;

        // The stretcher is always stereo. A mono buffer feeds both sides.
        std::array<const float*, maxChannels> in {};

        for (int channel = 0; channel < maxChannels; ++channel)
        {
            const auto source = juce::jmin (channel, numChannels - 1);
            stretcher->input.copyFrom (channel, 0, *fc.destBuffer, source, bufferStart, numSamples);
            in[(size_t) channel] = stretcher->input.getReadPointer (channel);
        }

        if (numChannels > 1)
        {
            std::array<float*, maxChannels> out { fc.destBuffer->getWritePointer (0, bufferStart),
                                                  fc.destBuffer->getWritePointer (1, bufferStart) };
            stretcher->stretch.process (in, numSamples, out, numSamples);
        }
        else
        {
            // ...and has nowhere for the right side to go, so that output is
            // written to a single float and forgotten.
            MonoOutput out { fc.destBuffer->getWritePointer (0, bufferStart) };
            stretcher->stretch.process (in, numSamples, out, numSamples);
        }
    }

    te::clearChannels (*fc.destBuffer, maxChannels, -1, fc.bufferStartSample, fc.bufferNumSamples);

    // What tracktion's did too: notes passing through follow the audio.
    if (fc.bufferForMidiMessages != nullptr)
        fc.bufferForMidiMessages->addToNoteNumbers (juce::roundToInt (semitones->getCurrentValue()));
}

void PitchShifterPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, semitonesValue);

    for (auto p : getAutomatableParameters())
        p->updateFromAttachedValue();
}

} // namespace carve::plugins
