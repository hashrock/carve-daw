#include "StereoImagerPlugin.h"

namespace carve::plugins
{

const char* StereoImagerPlugin::xmlTypeName = "carveStereoImager";

StereoImagerPlugin::StereoImagerPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    auto um = getUndoManager();
    widthValue.referTo (state, "width", um, 100.0f);
    monoBelowValue.referTo (state, "monoBelow", um, monoOff);

    width = addParam ("width", TRANS ("Width"), { 0.0f, 200.0f },
                      [] (float v) { return v <= 0.5f ? juce::String ("mono") : juce::String (juce::roundToInt (v)) + "%"; },
                      [] (const juce::String& s) { return s.getFloatValue(); });

    juce::NormalisableRange<float> hz { monoOff, 500.0f };
    hz.setSkewForCentre (120.0f);
    monoBelow = addParam ("monoBelow", TRANS ("Mono Below"), hz,
                          [] (float v) { return v <= monoOff + 0.5f ? juce::String ("off") : juce::String (juce::roundToInt (v)) + " Hz"; },
                          [] (const juce::String& s) { return s.getFloatValue(); });

    width->attachToCurrentValue (widthValue);
    monoBelow->attachToCurrentValue (monoBelowValue);
}

StereoImagerPlugin::~StereoImagerPlugin()
{
    notifyListenersOfDeletion();

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void StereoImagerPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, widthValue, monoBelowValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

void StereoImagerPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    fs = info.sampleRate;
    designedHz = -1.0f;
    widthNow = width->getCurrentValue() / 100.0f;
}

void StereoImagerPlugin::design (float hz)
{
    // A second-order Butterworth high-pass (RBJ, Q = 1/sqrt 2), twice.
    const double w0 = juce::MathConstants<double>::twoPi * juce::jmin ((double) hz, fs * 0.45) / fs;
    const double alpha = std::sin (w0) / (2.0 * (1.0 / juce::MathConstants<double>::sqrt2)), cw = std::cos (w0);
    const double a0 = 1.0 + alpha;

    for (auto& f : highPass)
    {
        f.b0 = (1.0 + cw) / 2.0 / a0;
        f.b1 = -(1.0 + cw) / a0;
        f.b2 = f.b0;
        f.a1 = -2.0 * cw / a0;
        f.a2 = (1.0 - alpha) / a0;
    }

    designedHz = hz;
}

void StereoImagerPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.destBuffer->getNumChannels() < 2)
        return;

    SCOPED_REALTIME_CHECK

    auto* l = fc.destBuffer->getWritePointer (0, fc.bufferStartSample);
    auto* r = fc.destBuffer->getWritePointer (1, fc.bufferStartSample);
    const int n = fc.bufferNumSamples;

    const auto cutoff = monoBelow->getCurrentValue();
    const bool monoBass = cutoff > monoOff + 0.5f;

    if (monoBass && std::abs (cutoff - designedHz) > 0.01f)
        design (cutoff);

    // The width glides to where it is set over the block.
    const auto target = width->getCurrentValue() / 100.0f;
    const auto step = (target - widthNow) / (float) juce::jmax (1, n);

    for (int i = 0; i < n; ++i)
    {
        widthNow += step;
        const double mid = 0.5 * ((double) l[i] + r[i]);
        double side = 0.5 * ((double) l[i] - r[i]);

        if (monoBass)
            side = highPass[1].process (highPass[0].process (side));

        side *= widthNow;
        l[i] = (float) (mid + side);
        r[i] = (float) (mid - side);
    }

    widthNow = target;
}

} // namespace carve::plugins
