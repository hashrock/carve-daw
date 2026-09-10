#include "LimiterPlugin.h"

namespace carve::plugins
{

namespace
{
    const juce::Identifier inputDbId ("inputDb"), ceilingDbId ("ceilingDb"), releaseMsId ("releaseMs");

    juce::String dbText (float v)               { return juce::String (v, 1) + " dB"; }
    float plainValue (const juce::String& s)    { return s.getFloatValue(); }

    juce::String msText (float v)
    {
        return v < 1000.0f ? juce::String (juce::roundToInt (v)) + " ms"
                           : juce::String (v * 0.001f, 2) + " s";
    }

    float msValue (const juce::String& s)
    {
        const auto v = s.getFloatValue();
        return s.containsIgnoreCase ("ms") ? v : v * 1000.0f;
    }

    inline float smoothingCoefficient (double seconds, double sampleRate) noexcept
    {
        return (float) (1.0 - std::exp (-1.0 / juce::jmax (1.0, seconds * sampleRate)));
    }
} // namespace

const char* LimiterPlugin::xmlTypeName = "carveLimiter";

//==============================================================================
void LimiterPlugin::MinWindow::resize (int windowLength)
{
    length = juce::jmax (1, windowLength);
    values.assign ((size_t) length + 1, 1.0f);
    indices.assign ((size_t) length + 1, 0);
    reset();
}

void LimiterPlugin::MinWindow::reset()
{
    head = tail = 0;
    position = 0;
}

float LimiterPlugin::MinWindow::push (float value) noexcept
{
    const auto capacity = (int) values.size();

    // Anything at the back that is no smaller than the new value can never be
    // the minimum again -- that is what keeps the deque monotonic, and short.
    while (tail != head)
    {
        const auto back = (tail - 1 + capacity) % capacity;

        if (values[(size_t) back] < value)
            break;

        tail = back;
    }

    values[(size_t) tail] = value;
    indices[(size_t) tail] = position;
    tail = (tail + 1) % capacity;

    // Drop the front once it has fallen out of the window.
    while (head != tail && indices[(size_t) head] <= position - length)
        head = (head + 1) % capacity;

    ++position;
    return values[(size_t) head];
}

//==============================================================================
LimiterPlugin::LimiterPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    auto um = getUndoManager();

    // A ceiling just under full scale, and a release slow enough not to
    // pump on a mix: the settings a safety limiter is left at.
    inputDbValue.referTo (state, inputDbId, um, 0.0f);
    ceilingDbValue.referTo (state, ceilingDbId, um, -0.3f);
    releaseMsValue.referTo (state, releaseMsId, um, 150.0f);

    inputDb   = addParam ("input",   TRANS ("Input"),   { -12.0f, 24.0f }, dbText, plainValue);
    ceilingDb = addParam ("ceiling", TRANS ("Ceiling"), { -12.0f, 0.0f }, dbText, plainValue);
    releaseMs = addParam ("release", TRANS ("Release"), { 10.0f, 1000.0f, 0.0f, 0.4f }, msText, msValue);

    inputDb->attachToCurrentValue (inputDbValue);
    ceilingDb->attachToCurrentValue (ceilingDbValue);
    releaseMs->attachToCurrentValue (releaseMsValue);
}

LimiterPlugin::~LimiterPlugin()
{
    notifyListenersOfDeletion();

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void LimiterPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    currentSampleRate = info.sampleRate;
    lookaheadSamples = juce::jmax (1, (int) std::ceil (lookaheadSeconds * currentSampleRate));

    // The only allocations, made here so applyToBuffer never has to.
    delayBuffer.setSize (maxChannels, lookaheadSamples + 1);
    delayBuffer.clear();
    delayWritePos = 0;

    minWindow.resize (lookaheadSamples);

    attackCoefficient = smoothingCoefficient (attackSeconds, currentSampleRate);
    currentGain = 1.0f;
    gainReductionDb.store (0.0f, std::memory_order_relaxed);
}

void LimiterPlugin::deinitialise()
{
    delayBuffer.clear();
    minWindow.reset();
}

double LimiterPlugin::getLatencySeconds()
{
    return lookaheadSamples / currentSampleRate;
}

void LimiterPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, inputDbValue, ceilingDbValue, releaseMsValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

//==============================================================================
void LimiterPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    const auto numChannels = juce::jmin (maxChannels, fc.destBuffer->getNumChannels());

    if (numChannels == 0 || delayBuffer.getNumSamples() == 0)
        return;

    const auto inputGain = (float) te::dbToGain (inputDb->getCurrentValue());
    const auto ceiling = (float) te::dbToGain (ceilingDb->getCurrentValue());

    releaseCoefficient = smoothingCoefficient ((double) releaseMs->getCurrentValue() * 0.001, currentSampleRate);

    const auto delayLength = delayBuffer.getNumSamples();
    auto lowestGain = 1.0f;

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        const auto sample = fc.bufferStartSample + i;

        // Stereo-linked: one peak, one gain, both channels.
        auto peak = 0.0f;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto x = fc.destBuffer->getSample (channel, sample) * inputGain;
            delayBuffer.setSample (channel, delayWritePos, x);
            peak = juce::jmax (peak, std::abs (x));
        }

        // What this sample alone would need, then the worst of everything
        // still inside the lookahead window.
        const auto wanted = peak > ceiling ? ceiling / peak : 1.0f;
        const auto target = minWindow.push (wanted);

        currentGain += (target - currentGain)
                        * (target < currentGain ? attackCoefficient : releaseCoefficient);

        lowestGain = juce::jmin (lowestGain, currentGain);

        // The sample the window was deciding about is the one arriving now.
        const auto readPos = (delayWritePos + 1) % delayLength;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto delayed = delayBuffer.getSample (channel, readPos) * currentGain;

            // The smoothing can leave a hair of overshoot on the sharpest
            // transients; this is what makes the ceiling a guarantee.
            fc.destBuffer->setSample (channel, sample, juce::jlimit (-ceiling, ceiling, delayed));
        }

        delayWritePos = readPos;
    }

    gainReductionDb.store (lowestGain < 1.0f ? -(float) te::gainToDb (lowestGain) : 0.0f,
                           std::memory_order_relaxed);

    for (int channel = numChannels; channel < fc.destBuffer->getNumChannels(); ++channel)
        fc.destBuffer->clear (channel, fc.bufferStartSample, fc.bufferNumSamples);
}

} // namespace carve::plugins
