#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::app
{

namespace meter
{
    constexpr float minDb = -60.0f;
    constexpr float maxDb = 6.0f;

    // dB per timer tick. At 30Hz this is a ~20dB/second fall-off, slow enough
    // to read a transient but fast enough to look live.
    constexpr float decayDb = 0.7f;

    inline float dbToProportion (float db)
    {
        return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
    }
} // namespace meter

//==============================================================================
// A stereo peak meter reading one LevelMeasurer: a pair of bars, upright in a
// mixer strip or lying down in the transport bar.
//
// Owns the LevelMeasurer::Client, which has to be registered with the measurer
// for levels to be measured at all (LevelMeasurer::processBuffer bails out when
// it has no clients). The measurer it watches is passed in on every tick rather
// than cached: a track's meter plugin is destroyed and rebuilt by EditSync when
// the generator's instrument changes, and the master's measurer lives in the
// playback context, which comes and goes with the transport.
class LevelMeterView : public juce::Component
{
public:
    enum class Orientation { vertical, horizontal };

    explicit LevelMeterView (Orientation o = Orientation::vertical) : orientation (o)
    {
        setInterceptsMouseClicks (false, false);
    }

    ~LevelMeterView() override  { attach (nullptr); }

    void update (te::LevelMeasurer* wanted)
    {
        attach (wanted);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto peak = measurer != nullptr ? client.getAndClearAudioLevel (channel).dB
                                                  : -100.0f;
            levelDb[channel] = std::max (peak, levelDb[channel] - meter::decayDb);
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();

        g.setColour (juce::Colour (0xff1c1c20));
        g.fillRect (area);

        const bool upright = orientation == Orientation::vertical;
        const auto channelSpan = (upright ? area.getWidth() : area.getHeight()) / (float) numChannels;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto proportion = meter::dbToProportion (levelDb[channel]);
            g.setColour (levelDb[channel] > 0.0f ? juce::Colours::orangered
                                                 : juce::Colour (0xff4fc27a));

            if (upright)
            {
                auto bar = area.withWidth (channelSpan)
                               .translated (channelSpan * (float) channel, 0.0f)
                               .reduced (1.0f, 0.0f);
                g.fillRect (bar.removeFromBottom (bar.getHeight() * proportion));
            }
            else
            {
                auto bar = area.withHeight (channelSpan)
                               .translated (0.0f, channelSpan * (float) channel)
                               .reduced (0.0f, 1.0f);
                g.fillRect (bar.removeFromLeft (bar.getWidth() * proportion));
            }
        }

        // 0dB mark
        g.setColour (juce::Colour (0xff707078));
        const auto zero = meter::dbToProportion (0.0f);

        if (upright)
            g.drawHorizontalLine ((int) (area.getBottom() - area.getHeight() * zero), area.getX(), area.getRight());
        else
            g.drawVerticalLine ((int) (area.getX() + area.getWidth() * zero), area.getY(), area.getBottom());
    }

private:
    static constexpr int numChannels = 2;

    void attach (te::LevelMeasurer* wanted)
    {
        if (wanted == measurer.get())
            return;

        if (auto* m = measurer.get())
            m->removeClient (client);

        measurer = wanted;

        if (wanted != nullptr)
            wanted->addClient (client);
    }

    Orientation orientation;

    // WeakReference: the measurer can be destroyed under us.
    juce::WeakReference<te::LevelMeasurer> measurer;
    te::LevelMeasurer::Client client;
    float levelDb[numChannels] { -100.0f, -100.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeterView)
};

} // namespace carve::app
