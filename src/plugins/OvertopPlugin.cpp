#include "OvertopPlugin.h"

namespace carve::plugins
{

namespace
{
    const juce::Identifier depthId ("depth"), lowMidHzId ("lowMidHz"), midHighHzId ("midHighHz"),
                           thresholdDbId ("thresholdDb"), downwardRatioId ("downwardRatio"),
                           upwardRatioId ("upwardRatio"), attackMsId ("attackMs"),
                           releaseMsId ("releaseMs"), lowGainDbId ("lowGainDb"),
                           midGainDbId ("midGainDb"), highGainDbId ("highGainDb"),
                           maxUpwardDbId ("maxUpwardDb"), outputDbId ("outputDb");

    // How often the gain is worked out from the envelope. The envelope itself
    // still moves every sample; this is only how often its level is turned
    // into a gain, which costs a log and an exponential per band. Sixteen
    // samples is a third of a millisecond -- far inside the shortest attack
    // the panel allows, and 16 times cheaper.
    constexpr int gainControlInterval = 16;

    // The gain is slewed towards its target over about this long, which is
    // what stops a jump in the envelope from arriving as a step in the audio.
    constexpr double gainSlewSeconds = 0.002;

    // Where the two slopes meet, they meet over this much dB rather than at a
    // corner: a signal parked on the threshold otherwise chatters between
    // being pushed up and pulled down.
    constexpr float kneeDb = 6.0f;

    // Below this the upward compression lets go, over the 20dB above it. A
    // room's noise floor is not a quiet part of the performance, and lifting
    // it by 30dB is how an upward compressor turns a pause into a hiss.
    constexpr float upwardFloorDb = -70.0f;
    constexpr float upwardFadeDb = 20.0f;

    // log(0) is not a level.
    constexpr float envelopeEpsilon = 1.0e-7f;

    inline float smoothingCoefficient (double seconds, double sampleRate) noexcept
    {
        return (float) (1.0 - std::exp (-1.0 / juce::jmax (1.0, seconds * sampleRate)));
    }

    juce::String percentText (float v)          { return juce::String (juce::roundToInt (v)) + "%"; }
    float percentValue (const juce::String& s)  { return s.getFloatValue(); }

    juce::String dbText (float v)               { return juce::String (v, 1) + " dB"; }
    float plainValue (const juce::String& s)    { return s.getFloatValue(); }

    juce::String ratioText (float v)            { return juce::String (v, 1) + " : 1"; }

    juce::String msText (float v)               { return juce::String (juce::roundToInt (v)) + " ms"; }
    float msValue (const juce::String& s)       { return s.getFloatValue(); }

    juce::String hzText (float v)
    {
        return v < 1000.0f ? juce::String (juce::roundToInt (v)) + " Hz"
                           : juce::String (v * 0.001f, 2) + " kHz";
    }

    float hzValue (const juce::String& s)
    {
        const auto v = s.getFloatValue();
        return s.containsIgnoreCase ("k") ? v * 1000.0f : v;
    }
} // namespace

const char* OvertopPlugin::xmlTypeName = "carveOvertop";

//==============================================================================
OvertopPlugin::BiquadCoefficients OvertopPlugin::butterworthLowPass (double hz, double sampleRate)
{
    const auto w = juce::MathConstants<double>::twoPi
                    * juce::jlimit (10.0, sampleRate * 0.45, hz) / sampleRate;
    const auto cosw = std::cos (w);
    const auto alpha = std::sin (w) / juce::MathConstants<double>::sqrt2;
    const auto a0 = 1.0 + alpha;

    OvertopPlugin::BiquadCoefficients c;
    c.b0 = (1.0 - cosw) * 0.5 / a0;
    c.b1 = (1.0 - cosw) / a0;
    c.b2 = c.b0;
    c.a1 = -2.0 * cosw / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

OvertopPlugin::BiquadCoefficients OvertopPlugin::butterworthHighPass (double hz, double sampleRate)
{
    const auto w = juce::MathConstants<double>::twoPi
                    * juce::jlimit (10.0, sampleRate * 0.45, hz) / sampleRate;
    const auto cosw = std::cos (w);
    const auto alpha = std::sin (w) / juce::MathConstants<double>::sqrt2;
    const auto a0 = 1.0 + alpha;

    OvertopPlugin::BiquadCoefficients c;
    c.b0 = (1.0 + cosw) * 0.5 / a0;
    c.b1 = -(1.0 + cosw) / a0;
    c.b2 = c.b0;
    c.a1 = -2.0 * cosw / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

//==============================================================================
OvertopPlugin::OvertopPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    auto um = getUndoManager();

    // A quarter wet, which thickens without announcing itself, over settings
    // aggressive enough that turning Depth up gets you the whole effect.
    depthValue.referTo (state, depthId, um, 25.0f);
    lowMidHzValue.referTo (state, lowMidHzId, um, 90.0f);
    midHighHzValue.referTo (state, midHighHzId, um, 2500.0f);
    thresholdDbValue.referTo (state, thresholdDbId, um, -30.0f);
    downwardRatioValue.referTo (state, downwardRatioId, um, 20.0f);
    upwardRatioValue.referTo (state, upwardRatioId, um, 4.0f);
    attackMsValue.referTo (state, attackMsId, um, 10.0f);
    releaseMsValue.referTo (state, releaseMsId, um, 100.0f);
    lowGainDbValue.referTo (state, lowGainDbId, um, 0.0f);
    midGainDbValue.referTo (state, midGainDbId, um, 0.0f);
    highGainDbValue.referTo (state, highGainDbId, um, 0.0f);
    maxUpwardDbValue.referTo (state, maxUpwardDbId, um, 30.0f);
    outputDbValue.referTo (state, outputDbId, um, 0.0f);

    const juce::NormalisableRange<float> bandGainRange { -24.0f, 24.0f };

    depth       = addParam ("depth",     TRANS ("Depth"),     { 0.0f, 100.0f }, percentText, percentValue);
    lowMidHz    = addParam ("lowMid",    TRANS ("Low/Mid"),   { 30.0f, 500.0f, 0.0f, 0.4f }, hzText, hzValue);
    midHighHz   = addParam ("midHigh",   TRANS ("Mid/High"),  { 500.0f, 12000.0f, 0.0f, 0.35f }, hzText, hzValue);
    thresholdDb = addParam ("threshold", TRANS ("Threshold"), { -60.0f, 0.0f }, dbText, plainValue);

    downwardRatio = addParam ("downRatio", TRANS ("Down Ratio"), { 1.0f, 40.0f, 0.0f, 0.5f }, ratioText, plainValue);
    upwardRatio   = addParam ("upRatio",   TRANS ("Up Ratio"),   { 1.0f, 20.0f, 0.0f, 0.5f }, ratioText, plainValue);

    attackMs  = addParam ("attack",  TRANS ("Attack"),  { 1.0f, 100.0f, 0.0f, 0.5f }, msText, msValue);
    releaseMs = addParam ("release", TRANS ("Release"), { 10.0f, 500.0f, 0.0f, 0.5f }, msText, msValue);

    lowGainDb  = addParam ("lowGain",  TRANS ("Low Gain"),  bandGainRange, dbText, plainValue);
    midGainDb  = addParam ("midGain",  TRANS ("Mid Gain"),  bandGainRange, dbText, plainValue);
    highGainDb = addParam ("highGain", TRANS ("High Gain"), bandGainRange, dbText, plainValue);

    maxUpwardDb = addParam ("maxUpward", TRANS ("Max Boost"), { 6.0f, 48.0f }, dbText, plainValue);
    outputDb    = addParam ("output",    TRANS ("Output"),    { -24.0f, 24.0f }, dbText, plainValue);

    depth->attachToCurrentValue (depthValue);
    lowMidHz->attachToCurrentValue (lowMidHzValue);
    midHighHz->attachToCurrentValue (midHighHzValue);
    thresholdDb->attachToCurrentValue (thresholdDbValue);
    downwardRatio->attachToCurrentValue (downwardRatioValue);
    upwardRatio->attachToCurrentValue (upwardRatioValue);
    attackMs->attachToCurrentValue (attackMsValue);
    releaseMs->attachToCurrentValue (releaseMsValue);
    lowGainDb->attachToCurrentValue (lowGainDbValue);
    midGainDb->attachToCurrentValue (midGainDbValue);
    highGainDb->attachToCurrentValue (highGainDbValue);
    maxUpwardDb->attachToCurrentValue (maxUpwardDbValue);
    outputDb->attachToCurrentValue (outputDbValue);
}

OvertopPlugin::~OvertopPlugin()
{
    notifyListenersOfDeletion();

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void OvertopPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    currentSampleRate = info.sampleRate;

    smoothedLowMidHz = lowMidHz->getCurrentValue();
    smoothedMidHighHz = midHighHz->getCurrentValue();

    for (auto& splitter : splitters)
        splitter.reset();

    for (auto& band : dynamics)
        band.reset();
}

void OvertopPlugin::deinitialise()
{
}

void OvertopPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, depthValue, lowMidHzValue, midHighHzValue,
                                      thresholdDbValue, downwardRatioValue, upwardRatioValue,
                                      attackMsValue, releaseMsValue, lowGainDbValue,
                                      midGainDbValue, highGainDbValue, maxUpwardDbValue,
                                      outputDbValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

//==============================================================================
OvertopPlugin::BlockSettings OvertopPlugin::readSettings()
{
    BlockSettings s;

    // A block's worth of movement towards wherever the crossover knobs are
    // now, so a sweep is a sweep rather than a series of steps.
    const auto glide = 0.25;
    smoothedLowMidHz += (lowMidHz->getCurrentValue() - smoothedLowMidHz) * glide;
    smoothedMidHighHz += (juce::jmax ((float) smoothedLowMidHz * 1.5f, midHighHz->getCurrentValue())
                           - smoothedMidHighHz) * glide;

    s.lowPassBass  = butterworthLowPass (smoothedLowMidHz, currentSampleRate);
    s.highPassBass = butterworthHighPass (smoothedLowMidHz, currentSampleRate);
    s.lowPassSplit  = butterworthLowPass (smoothedMidHighHz, currentSampleRate);
    s.highPassSplit = butterworthHighPass (smoothedMidHighHz, currentSampleRate);

    s.attackCoefficient = smoothingCoefficient ((double) attackMs->getCurrentValue() * 0.001, currentSampleRate);
    s.releaseCoefficient = smoothingCoefficient ((double) releaseMs->getCurrentValue() * 0.001, currentSampleRate);
    s.gainSlew = smoothingCoefficient (gainSlewSeconds, currentSampleRate);

    s.thresholdDb = thresholdDb->getCurrentValue();

    // Kept as slopes rather than ratios: the per-sample maths wants the
    // reciprocal, and a ratio of 1 has to come out as "no change" rather than
    // as a division.
    s.downwardSlope = 1.0f / juce::jmax (1.0f, downwardRatio->getCurrentValue());
    s.upwardSlope   = 1.0f / juce::jmax (1.0f, upwardRatio->getCurrentValue());
    s.maxUpwardDb = maxUpwardDb->getCurrentValue();

    s.bandGain[0] = (float) te::dbToGain (lowGainDb->getCurrentValue());
    s.bandGain[1] = (float) te::dbToGain (midGainDb->getCurrentValue());
    s.bandGain[2] = (float) te::dbToGain (highGainDb->getCurrentValue());

    s.depth = depth->getCurrentValue() * 0.01f;
    s.outputGain = (float) te::dbToGain (outputDb->getCurrentValue());

    return s;
}

float OvertopPlugin::gainForLevel (float levelDb, float instantLevelDb, const BlockSettings& s) const noexcept
{
    // Both slopes, blended across the knee. Outside it the blend has settled
    // on one or the other, so this is the straight line the panel promises.
    auto curve = [&s] (float atLevelDb)
    {
        const auto over = atLevelDb - s.thresholdDb;
        const auto downward = over * s.downwardSlope;
        const auto upward = over * s.upwardSlope;

        const auto t = juce::jlimit (0.0f, 1.0f, (over + kneeDb * 0.5f) / kneeDb);
        const auto smooth = t * t * (3.0f - 2.0f * t);      // zero slope at both ends

        return (upward + (downward - upward) * smooth) - over;
    };

    auto gainDb = curve (levelDb);

    if (gainDb > 0.0f)
    {
        gainDb = juce::jmin (gainDb, s.maxUpwardDb);

        // Nothing at all down where only the noise floor lives...
        if (levelDb < upwardFloorDb + upwardFadeDb)
            gainDb *= juce::jlimit (0.0f, 1.0f, (levelDb - upwardFloorDb) / upwardFadeDb);

        // ...and no more than the band's own instant level still allows. On a
        // transient this drops to nothing straight away, so what arrives is
        // the transient rather than an amplified copy of it.
        gainDb = juce::jmin (gainDb, juce::jmax (0.0f, curve (instantLevelDb)));
    }

    return gainDb;
}

//==============================================================================
void OvertopPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    const auto numChannels = juce::jmin (maxChannels, fc.destBuffer->getNumChannels());

    if (numChannels == 0)
        return;

    const auto settings = readSettings();

    auto* left = fc.destBuffer->getWritePointer (0, fc.bufferStartSample);
    auto* right = numChannels > 1 ? fc.destBuffer->getWritePointer (1, fc.bufferStartSample) : nullptr;

    int untilGainUpdate = 0;

    for (int i = 0; i < fc.bufferNumSamples; ++i)
    {
        double bandL[numBands] {}, bandR[numBands] {};

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto& split = splitters[channel];
            const auto x = (double) (channel == 0 ? left[i] : right[i]);

            // Split the top off first, then the bottom out of what is left.
            const auto lowMid = split.lowOfSplit.process (settings.lowPassSplit, x);
            const auto high = split.highOfSplit.process (settings.highPassSplit, x);

            const auto low = split.lowOfBass.process (settings.lowPassBass, lowMid);
            const auto mid = split.highOfBass.process (settings.highPassBass, lowMid);

            // The high band has been through one crossover and the other two
            // through both, so it goes through the low crossover as well --
            // its two halves summed, which is an allpass. Without this the
            // three do not add back up.
            const auto alignedHigh = split.alignLow.process (settings.lowPassBass, high)
                                      + split.alignHigh.process (settings.highPassBass, high);

            auto* target = channel == 0 ? bandL : bandR;
            target[0] = low;
            target[1] = mid;
            target[2] = alignedHigh;
        }

        const auto updateGains = untilGainUpdate <= 0;

        if (updateGains)
            untilGainUpdate = gainControlInterval;

        --untilGainUpdate;

        double dryL = 0.0, dryR = 0.0, wetL = 0.0, wetR = 0.0;

        for (int band = 0; band < numBands; ++band)
        {
            auto& dynamic = dynamics[band];

            // Stereo-linked: one detector for both sides, or a loud left would
            // pull the image over every time it moved.
            const auto rectified = (float) juce::jmax (std::abs (bandL[band]),
                                                       numChannels > 1 ? std::abs (bandR[band]) : 0.0);

            dynamic.envelope += (rectified - dynamic.envelope)
                                 * (rectified > dynamic.envelope ? settings.attackCoefficient
                                                                 : settings.releaseCoefficient);

            // Straight up, and down at the release: the detector that decides
            // whether a boost is still welcome.
            dynamic.fastEnvelope = juce::jmax (rectified,
                                               dynamic.fastEnvelope
                                                + (rectified - dynamic.fastEnvelope) * settings.releaseCoefficient);

            if (updateGains)
            {
                const auto levelDb = (float) te::gainToDb (dynamic.envelope + envelopeEpsilon);
                const auto instantDb = (float) te::gainToDb (dynamic.fastEnvelope + envelopeEpsilon);

                dynamic.targetGain = (float) te::dbToGain (gainForLevel (levelDb, instantDb, settings))
                                      * settings.bandGain[(size_t) band];
            }

            dynamic.gain += (dynamic.targetGain - dynamic.gain) * settings.gainSlew;

            dryL += bandL[band];
            wetL += bandL[band] * dynamic.gain;

            if (right != nullptr)
            {
                dryR += bandR[band];
                wetR += bandR[band] * dynamic.gain;
            }
        }

        // The dry side is the bands summed *unprocessed*, not the input: an
        // LR4 split adds back up to an allpass, and mixing that against the
        // untouched input would comb rather than crossfade.
        left[i] = (float) ((dryL + (wetL - dryL) * settings.depth) * settings.outputGain);

        if (right != nullptr)
            right[i] = (float) ((dryR + (wetR - dryR) * settings.depth) * settings.outputGain);
    }

    for (int channel = numChannels; channel < fc.destBuffer->getNumChannels(); ++channel)
        fc.destBuffer->clear (channel, fc.bufferStartSample, fc.bufferNumSamples);
}

} // namespace carve::plugins
