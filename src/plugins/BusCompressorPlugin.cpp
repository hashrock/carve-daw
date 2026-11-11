#include "BusCompressorPlugin.h"

namespace carve::plugins
{

const char* BusCompressorPlugin::xmlTypeName = "carveBusCompressor";

BusCompressorPlugin::BusCompressorPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    auto um = getUndoManager();
    compThresholdValue.referTo (state, "threshold", um, -18.0f);
    compRatioValue.referTo (state, "ratio", um, 4.0f);
    compAttackValue.referTo (state, "attack", um, 10.0f);
    compReleaseValue.referTo (state, "release", um, 120.0f);
    compMakeupValue.referTo (state, "makeup", um, 0.0f);
    compMixValue.referTo (state, "mix", um, 1.0f);

    auto plain = [] (const juce::String& s) { return s.getFloatValue(); };
    auto db = [] (float v) { return juce::String (v, 1) + " dB"; };
    auto ms = [] (float v) { return v < 10.0f ? juce::String (v, 1) + " ms" : juce::String (juce::roundToInt (v)) + " ms"; };

    juce::NormalisableRange<float> ratioRange { 1.0f, 20.0f };
    ratioRange.setSkewForCentre (4.0f);
    juce::NormalisableRange<float> attackRange { 0.1f, 100.0f };
    attackRange.setSkewForCentre (10.0f);
    juce::NormalisableRange<float> releaseRange { 10.0f, 1000.0f };
    releaseRange.setSkewForCentre (120.0f);

    compThreshold = addParam ("threshold", TRANS ("Threshold"), { -40.0f, 0.0f }, db, plain);
    compRatio = addParam ("ratio", TRANS ("Ratio"), ratioRange,
                          [] (float v) { return juce::String (v, v < 10.0f ? 1 : 0) + ":1"; }, plain);
    compAttack = addParam ("attack", TRANS ("Attack"), attackRange, ms, plain);
    compRelease = addParam ("release", TRANS ("Release"), releaseRange, ms, plain);
    compMakeup = addParam ("makeup", TRANS ("Makeup"), { 0.0f, 24.0f }, db, plain);
    compMix = addParam ("mix", TRANS ("Mix"), { 0.0f, 1.0f },
                        [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "% wet"; },
                        [] (const juce::String& s) { return s.getFloatValue() * 0.01f; });

    compThreshold->attachToCurrentValue (compThresholdValue);
    compRatio->attachToCurrentValue (compRatioValue);
    compAttack->attachToCurrentValue (compAttackValue);
    compRelease->attachToCurrentValue (compReleaseValue);
    compMakeup->attachToCurrentValue (compMakeupValue);
    compMix->attachToCurrentValue (compMixValue);
}

BusCompressorPlugin::~BusCompressorPlugin()
{
    notifyListenersOfDeletion();

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void BusCompressorPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, compThresholdValue, compRatioValue, compAttackValue, compReleaseValue,
                                      compMakeupValue, compMixValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

fx::BusCompressor::Settings BusCompressorPlugin::getCompressorSettings() const
{
    fx::BusCompressor::Settings s;
    s.thresholdDb = compThreshold->getCurrentValue();
    s.ratio = compRatio->getCurrentValue();
    s.attackMs = compAttack->getCurrentValue();
    s.releaseMs = compRelease->getCurrentValue();
    s.makeupDb = compMakeup->getCurrentValue();
    s.mix = compMix->getCurrentValue();
    return s;
}

void BusCompressorPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    compressor.prepare (info.sampleRate);
}

void BusCompressorPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    auto& buffer = *fc.destBuffer;
    const int numChannels = juce::jmin (2, buffer.getNumChannels());

    if (numChannels == 0)
        return;

    float* channels[2] {};
    for (int ch = 0; ch < numChannels; ++ch)
        channels[ch] = buffer.getWritePointer (ch, fc.bufferStartSample);

    compressor.process (channels, numChannels, fc.bufferNumSamples, getCompressorSettings(), &history);
}

} // namespace carve::plugins
