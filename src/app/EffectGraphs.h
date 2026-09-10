#pragma once

#include <array>
#include <memory>

#include <tracktion_engine/tracktion_engine.h>

#include "ParameterRows.h"
#include "../plugins/OvertopPlugin.h"

namespace te = tracktion;

namespace carve::app
{

// The picture at the top of an effect's window, for the effects where a list
// of sliders is the wrong way to say what the effect is doing.
//
// A 4-band EQ is the obvious one: twelve numbers describing a shape nobody can
// read as a shape. The window keeps its sliders -- they are how a value gets
// typed exactly -- and puts the shape above them, where it can be dragged.
//
// Which effects get one, and what it draws, is createEffectGraph's business;
// the window only knows that some plugins come with a picture and how tall it
// is.
struct EffectGraph
{
    std::unique_ptr<juce::Component> component;
    int height = 0;

    explicit operator bool() const  { return component != nullptr; }
};

EffectGraph createEffectGraph (te::Plugin&);

//==============================================================================
// The EQ's response curve, with a handle per band.
//
// The curve is the plugin's own (getDBGainAtFrequency, which it computes from
// the very filters it is running), so what is drawn is what is being heard
// rather than a second implementation that can drift from it.
//
// Drag a handle for frequency and gain, wheel over one for Q, double-click to
// flatten that band. The frequency axis is logarithmic because hearing is: an
// octave takes the same width whether it is the one under 40Hz or the one
// under 10kHz.
class EqualiserGraph : public juce::Component,
                       private juce::Timer
{
public:
    explicit EqualiserGraph (te::EqualiserPlugin&);

    static constexpr int preferredHeight = 168;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void timerCallback() override;

    // One of the four bands, as the panel talks about it.
    struct Band
    {
        const char* name;
        te::AutomatableParameter::Ptr frequency, gain, q;
    };

    std::array<Band, 4> bands();

    float frequencyToX (float hz) const;
    float xToFrequency (float x) const;
    float gainToY (float db) const;
    float yToGain (float y) const;

    // The band whose handle is within grabbing distance of a point, or -1.
    int bandAt (juce::Point<float>) const;

    juce::Rectangle<float> plotArea() const;

    te::SafeSelectable<te::EqualiserPlugin> plugin;

    int draggedBand = -1;
    int hoveredBand = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqualiserGraph)
};

//==============================================================================
// Overtop's three bands: what each one is hearing, and what the threshold is
// doing to it.
//
// A threshold is the one control on a compressor that cannot be set from a
// number: -30dB means nothing until you can see where the signal sits against
// it. So each band gets a column -- its level as a bar, the gain it is being
// given as a mark beside it -- and the threshold is a line across all three
// that can be dragged, since it is one setting shared by the bands.
//
// Above the line the band is being pushed down, below it pushed up, and the
// column is drawn in the colour of whichever is happening: that is the whole
// idea of the effect, and it is not visible anywhere else.
class OvertopGraph : public juce::Component,
                     private juce::Timer
{
public:
    explicit OvertopGraph (plugins::OvertopPlugin&);

    static constexpr int preferredHeight = 172;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;

    float levelToY (float db) const;
    float yToLevel (float y) const;
    juce::Rectangle<float> plotArea() const;
    juce::Rectangle<float> columnFor (int band) const;

    te::SafeSelectable<plugins::OvertopPlugin> plugin;

    // Peak-holding what the meter shows, so a transient is readable rather
    // than a flicker between two repaints.
    std::array<float, 3> shownLevelDb { -100.0f, -100.0f, -100.0f };
    std::array<float, 3> shownGainDb { 0.0f, 0.0f, 0.0f };

    bool draggingThreshold = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OvertopGraph)
};

} // namespace carve::app
