#include "DelayPlugin.h"

namespace carve::plugins
{

namespace
{
    const juce::Identifier syncId ("sync"), timeMsId ("timeMs"), offsetId ("offset"),
                           feedbackId ("feedback"), pingPongId ("pingPong"),
                           lowCutId ("lowCut"), highCutId ("highCut"),
                           modRateId ("modRate"), modDepthId ("modDepth"),
                           widthId ("width"), mixId ("mix"), outputDbId ("outputDb");

    struct SyncDivision
    {
        const char* name;
        double beats;
    };

    // Index 0 is Free; the rest are in order of length, so the Sync knob
    // sweeps from the fastest repeat to the slowest.
    const SyncDivision syncDivisions[]
    {
        { "Free",  0.0        },
        { "1/16",  0.25       },
        { "1/8T",  1.0 / 3.0  },
        { "1/8",   0.5        },
        { "1/4T",  2.0 / 3.0  },
        { "1/8.",  0.75       },
        { "1/4",   1.0        },
        { "1/4.",  1.5        },
        { "1/2",   2.0        },
    };

    constexpr int numSyncDivisions = (int) (sizeof (syncDivisions) / sizeof (syncDivisions[0]));

    // How far the LFO can pull the read position at full depth. Twelve
    // milliseconds is a wide chorus at a short delay time and a convincing
    // tape flutter at a long one; more than this and short times run into
    // the front of the line.
    constexpr double maxModMilliseconds = 12.0;

    // The delay time takes about this long to reach a new setting. Long
    // enough to bend rather than step, short enough that switching sync
    // divisions lands before the next repeat.
    constexpr double delayGlideSeconds = 0.09;

    // Never read the sample that is being written, and leave room for the
    // interpolator's neighbours at either end.
    constexpr double minDelaySamples = 4.0;

    inline float onePoleCoefficient (double hz, double sampleRate) noexcept
    {
        const auto x = std::exp (-juce::MathConstants<double>::twoPi
                                  * juce::jlimit (1.0, sampleRate * 0.45, hz) / sampleRate);
        return (float) (1.0 - x);
    }

    juce::String percentText (float v)          { return juce::String (juce::roundToInt (v)) + "%"; }
    float percentValue (const juce::String& s)  { return s.getFloatValue(); }

    juce::String hzText (float v)
    {
        return v < 1000.0f ? juce::String (v, v < 10.0f ? 2 : 0) + " Hz"
                           : juce::String (v * 0.001f, 2) + " kHz";
    }

    float hzValue (const juce::String& s)
    {
        const auto v = s.getFloatValue();
        return s.containsIgnoreCase ("k") ? v * 1000.0f : v;
    }

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

    juce::String dbText (float v)               { return juce::String (v, 1) + " dB"; }
    float plainValue (const juce::String& s)    { return s.getFloatValue(); }
} // namespace

const char* DelayPlugin::xmlTypeName = "carveDelay";

int DelayPlugin::getNumSyncDivisions()                      { return numSyncDivisions; }
const char* DelayPlugin::getSyncDivisionName (int index)    { return syncDivisions[juce::jlimit (0, numSyncDivisions - 1, index)].name; }
double DelayPlugin::getSyncDivisionBeats (int index)        { return syncDivisions[juce::jlimit (0, numSyncDivisions - 1, index)].beats; }

//==============================================================================
void DelayPlugin::Line::resize (int numSamples)
{
    buffer.assign ((size_t) juce::jmax (16, numSamples), 0.0f);
    writePos = 0;
}

void DelayPlugin::Line::clear()
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    writePos = 0;
}

void DelayPlugin::Line::write (float x) noexcept
{
    if (buffer.empty())
        return;

    buffer[(size_t) writePos] = x;

    if (++writePos >= (int) buffer.size())
        writePos = 0;
}

float DelayPlugin::Line::read (double delaySamples) const noexcept
{
    const auto size = (int) buffer.size();

    if (size == 0)
        return 0.0f;

    auto position = (double) writePos - delaySamples;

    while (position < 0.0)
        position += size;

    const auto index = (int) position;
    const auto fraction = position - index;

    auto at = [this, size] (int i) noexcept
    {
        while (i >= size) i -= size;
        while (i < 0)     i += size;
        return (double) buffer[(size_t) i];
    };

    // Catmull-Rom through the four samples around the read position.
    const auto x0 = at (index - 1), x1 = at (index), x2 = at (index + 1), x3 = at (index + 2);
    const auto a = 0.5 * (x3 - x0) + 1.5 * (x1 - x2);
    const auto b = x0 - 2.5 * x1 + 2.0 * x2 - 0.5 * x3;
    const auto c = 0.5 * (x2 - x0);

    return (float) (((a * fraction + b) * fraction + c) * fraction + x1);
}

//==============================================================================
DelayPlugin::DelayPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    auto um = getUndoManager();

    // An eighth-note delay with a third of it coming back, rolled off top and
    // bottom: the setting a delay is reached for nine times out of ten.
    syncValue.referTo (state, syncId, um, 3.0f);
    timeMsValue.referTo (state, timeMsId, um, 375.0f);
    offsetValue.referTo (state, offsetId, um, 100.0f);
    feedbackValue.referTo (state, feedbackId, um, 35.0f);
    pingPongValue.referTo (state, pingPongId, um, 0.0f);
    lowCutValue.referTo (state, lowCutId, um, 120.0f);
    highCutValue.referTo (state, highCutId, um, 6000.0f);
    modRateValue.referTo (state, modRateId, um, 0.6f);
    modDepthValue.referTo (state, modDepthId, um, 0.0f);
    widthValue.referTo (state, widthId, um, 100.0f);
    mixValue.referTo (state, mixId, um, 25.0f);
    outputDbValue.referTo (state, outputDbId, um, 0.0f);

    const juce::NormalisableRange<float> percent { 0.0f, 100.0f };

    // Stepped, and named rather than numbered: the value string is what the
    // editor's row shows, so the slider reads "1/8." instead of "5".
    sync = addParam ("sync", TRANS ("Sync"), { 0.0f, (float) (numSyncDivisions - 1), 1.0f },
                     [] (float v)               { return juce::String (getSyncDivisionName (juce::roundToInt (v))); },
                     [] (const juce::String& s)
                     {
                         for (int i = 0; i < numSyncDivisions; ++i)
                             if (s.trim().equalsIgnoreCase (getSyncDivisionName (i)))
                                 return (float) i;

                         return 0.0f;
                     });

    timeMs   = addParam ("time",     TRANS ("Time"),      { 10.0f, 2000.0f, 0.0f, 0.35f }, msText, msValue);
    offset   = addParam ("offset",   TRANS ("R Offset"),  { 50.0f, 150.0f }, percentText, percentValue);
    feedback = addParam ("feedback", TRANS ("Feedback"),  percent, percentText, percentValue);
    pingPong = addParam ("pingPong", TRANS ("Ping-Pong"), percent, percentText, percentValue);

    lowCut  = addParam ("lowCut",  TRANS ("Low Cut"),  { 20.0f, 2000.0f, 0.0f, 0.35f }, hzText, hzValue);
    highCut = addParam ("highCut", TRANS ("High Cut"), { 200.0f, 20000.0f, 0.0f, 0.3f }, hzText, hzValue);

    modRate  = addParam ("modRate",  TRANS ("Mod Rate"),  { 0.02f, 8.0f, 0.0f, 0.4f },
                         [] (float v)               { return juce::String (v, 2) + " Hz"; }, plainValue);
    modDepth = addParam ("modDepth", TRANS ("Mod Depth"), percent, percentText, percentValue);

    width    = addParam ("width",  TRANS ("Width"),  percent, percentText, percentValue);
    mix      = addParam ("mix",    TRANS ("Mix"),    percent, percentText, percentValue);
    outputDb = addParam ("output", TRANS ("Output"), { -24.0f, 6.0f }, dbText, plainValue);

    sync->attachToCurrentValue (syncValue);
    timeMs->attachToCurrentValue (timeMsValue);
    offset->attachToCurrentValue (offsetValue);
    feedback->attachToCurrentValue (feedbackValue);
    pingPong->attachToCurrentValue (pingPongValue);
    lowCut->attachToCurrentValue (lowCutValue);
    highCut->attachToCurrentValue (highCutValue);
    modRate->attachToCurrentValue (modRateValue);
    modDepth->attachToCurrentValue (modDepthValue);
    width->attachToCurrentValue (widthValue);
    mix->attachToCurrentValue (mixValue);
    outputDb->attachToCurrentValue (outputDbValue);
}

DelayPlugin::~DelayPlugin()
{
    notifyListenersOfDeletion();

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void DelayPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    currentSampleRate = info.sampleRate;

    // The only allocation, made here so applyToBuffer never has to.
    const auto lineLength = (int) std::ceil (maxDelaySeconds * currentSampleRate) + 8;

    for (auto& line : lines)
        line.resize (lineLength);

    for (auto& filter : lowCutFilter)
        filter.reset();

    for (auto& filter : highCutFilter)
        filter.reset();

    delaySlew = 1.0 - std::exp (-1.0 / (delayGlideSeconds * currentSampleRate));

    targetDelayL = targetDelayR = currentDelayL = currentDelayR = minDelaySamples;
    lfoPhase = 0.0;
}

void DelayPlugin::deinitialise()
{
    for (auto& line : lines)
        line.clear();
}

void DelayPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, syncValue, timeMsValue, offsetValue, feedbackValue,
                                      pingPongValue, lowCutValue, highCutValue, modRateValue,
                                      modDepthValue, widthValue, mixValue, outputDbValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

//==============================================================================
void DelayPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    const auto numChannels = juce::jmin (maxChannels, fc.destBuffer->getNumChannels());

    if (numChannels == 0 || lines[0].buffer.empty())
        return;

    // Where the repeats land. A sync division is in beats, so it follows a
    // tempo change the same way a clip does; Free hands it to the Time knob.
    const auto divisionIndex = juce::roundToInt (sync->getCurrentValue());
    auto delaySeconds = (double) timeMs->getCurrentValue() * 0.001;

    if (divisionIndex > 0)
    {
        tempoPosition.set (fc.editTime.getStart());
        const auto bpm = juce::jmax (1.0, tempoPosition.getTempo());
        delaySeconds = getSyncDivisionBeats (divisionIndex) * 60.0 / bpm;
    }

    const auto maxSamples = (double) lines[0].buffer.size() - 8.0;
    const auto offsetScale = (double) offset->getCurrentValue() * 0.01;

    targetDelayL = juce::jlimit (minDelaySamples, maxSamples, delaySeconds * currentSampleRate);
    targetDelayR = juce::jlimit (minDelaySamples, maxSamples, targetDelayL * offsetScale);

    const auto feedbackAmount = (double) feedback->getCurrentValue() * 0.01;
    const auto ping = (double) pingPong->getCurrentValue() * 0.01;
    const auto wetAmount = (double) mix->getCurrentValue() * 0.01;
    const auto dryAmount = 1.0 - wetAmount;
    const auto widthAmount = (double) width->getCurrentValue() * 0.01;
    const auto outputGain = (double) te::dbToGain (outputDb->getCurrentValue());

    const auto modSamples = (double) modDepth->getCurrentValue() * 0.01
                             * maxModMilliseconds * 0.001 * currentSampleRate;
    const auto lfoIncrement = (double) modRate->getCurrentValue() / currentSampleRate;

    for (int channel = 0; channel < maxChannels; ++channel)
    {
        lowCutFilter[channel].setCoefficient (onePoleCoefficient (lowCut->getCurrentValue(), currentSampleRate));
        highCutFilter[channel].setCoefficient (onePoleCoefficient (highCut->getCurrentValue(), currentSampleRate));
    }

    auto* left = fc.destBuffer->getWritePointer (0, fc.bufferStartSample);
    auto* right = numChannels > 1 ? fc.destBuffer->getWritePointer (1, fc.bufferStartSample) : nullptr;

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        currentDelayL += (targetDelayL - currentDelayL) * delaySlew;
        currentDelayR += (targetDelayR - currentDelayR) * delaySlew;

        // The two channels sit a quarter cycle apart, so the wobble widens
        // the image rather than moving both sides together.
        const auto modL = modSamples * std::sin (lfoPhase * juce::MathConstants<double>::twoPi);
        const auto modR = modSamples * std::sin ((lfoPhase + 0.25) * juce::MathConstants<double>::twoPi);

        lfoPhase += lfoIncrement;

        if (lfoPhase >= 1.0)
            lfoPhase -= std::floor (lfoPhase);

        const auto inL = (double) left[i];
        const auto inR = right != nullptr ? (double) right[i] : inL;

        const auto wetL = (double) lines[0].read (juce::jlimit (minDelaySamples, maxSamples, currentDelayL + modL));
        const auto wetR = (double) lines[1].read (juce::jlimit (minDelaySamples, maxSamples, currentDelayR + modR));

        // The repeats lose their extremes on every trip round the loop, which
        // is what keeps a long feedback from turning into mud.
        const auto fbL = (double) highCutFilter[0].lowPass (lowCutFilter[0].highPass ((float) (wetL * feedbackAmount)));
        const auto fbR = (double) highCutFilter[1].lowPass (lowCutFilter[1].highPass ((float) (wetR * feedbackAmount)));

        // Ping-pong, as a crossfade rather than a switch: at 1 the input is
        // summed into the left line only and each side feeds the other, which
        // is the bounce; at 0 the two are separate delays.
        const auto mono = 0.5 * (inL + inR);
        const auto srcL = inL * (1.0 - ping) + mono * ping;
        const auto srcR = inR * (1.0 - ping);

        lines[0].write ((float) (srcL + fbL * (1.0 - ping) + fbR * ping));
        lines[1].write ((float) (srcR + fbR * (1.0 - ping) + fbL * ping));

        // Width on the wet only: the dry signal is whatever came in.
        const auto mid = 0.5 * (wetL + wetR);
        const auto side = 0.5 * (wetL - wetR) * widthAmount;

        left[i] = (float) ((inL * dryAmount + (mid + side) * wetAmount) * outputGain);

        if (right != nullptr)
            right[i] = (float) ((inR * dryAmount + (mid - side) * wetAmount) * outputGain);
    }

    for (int channel = numChannels; channel < fc.destBuffer->getNumChannels(); ++channel)
        fc.destBuffer->clear (channel, fc.bufferStartSample, fc.bufferNumSamples);
}

} // namespace carve::plugins
