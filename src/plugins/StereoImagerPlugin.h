#pragma once

#include <array>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins
{

// The stereo image, for the master and the mastering chain: Width scales the
// side signal (0% mono, 100% as it is, 200% twice as wide), and below Mono
// Below the side is taken out altogether -- the low end in the middle, where
// a club's subs and a vinyl cutter both want it, and where a wide bass is
// mostly phase trouble.
//
// The side is split with a Linkwitz-Riley crossover (fourth order) and only
// its high band kept; the mid passes untouched, so a mono fold-down of the
// output is the mono fold-down of the input whatever the settings.
class StereoImagerPlugin : public te::Plugin
{
public:
    explicit StereoImagerPlugin (te::PluginCreationInfo);
    ~StereoImagerPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Stereo Imager"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Stereo Imager"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Imager"; }
    juce::String getSelectableDescription() override  { return TRANS ("Stereo Imager"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override;
    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    juce::CachedValue<float> widthValue, monoBelowValue;
    te::AutomatableParameter::Ptr width, monoBelow;   // percent, Hz (20 = off)

    static constexpr float monoOff = 20.0f;

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

        double process (double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    void design (float hz);

    std::array<Biquad, 2> highPass;   // two Butterworths: Linkwitz-Riley
    float designedHz = -1.0f;
    double fs = 48000.0;
    float widthNow = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoImagerPlugin)
};

} // namespace carve::plugins
