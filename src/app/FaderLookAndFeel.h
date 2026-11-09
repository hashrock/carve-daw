#pragma once

#include <cmath>
#include <limits>

#include <tracktion_engine/tracktion_engine.h>

#include "Fonts.h"

namespace carve::app
{

//==============================================================================
// The mixer's sliders, drawn as the hardware the knobs (RotaryDial) already
// look like: a cap riding in a slot cut into the panel, lit from above.
//
// A vertical slider is a channel fader -- the mixer has no other kind -- in
// tracktion's fader-position space, so its scale is marked in decibels at the
// positions tracktion puts them. A horizontal one is a pan or a send: a small
// cap in a short slot. Either way the stretch of slot between the zero point
// and the cap glows in the app's orange, like the swept marks round a knob,
// from the middle for a bipolar range (pan), from the bottom or left for the
// rest.
//
// Everything that is not a linear slider is drawn as CarveLookAndFeel draws
// it, so the strips' buttons, labels and menus are unchanged.
class FaderLookAndFeel : public CarveLookAndFeel
{
public:
    int getSliderThumbRadius (juce::Slider& slider) override
    {
        // How far the travel stops short of the ends: half a cap, so the cap
        // never hangs out of its slot at either extreme.
        return slider.isVertical() ? faderCapHeight / 2 : panCapWidth / 2;
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearHorizontal)
        {
            CarveLookAndFeel::drawLinearSlider (g, x, y, width, height, sliderPos,
                                                minSliderPos, maxSliderPos, style, slider);
            return;
        }

        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
        const auto alpha = slider.isEnabled() ? 1.0f : 0.4f;

        if (style == juce::Slider::LinearVertical)
            drawFader (g, bounds, sliderPos, slider, alpha);
        else
            drawSmallSlider (g, bounds, sliderPos, slider, alpha);
    }

private:
    static constexpr int faderCapHeight = 30;
    static constexpr int panCapWidth = 12;
    static constexpr juce::uint32 accentColour = 0xffe0a24f;   // the app's orange, as on the knobs

    // The proportion of the travel the range's zero sits at: the middle of a
    // bipolar range, otherwise the bottom (or left) end.
    static double zeroProportion (juce::Slider& slider)
    {
        const auto min = slider.getMinimum(), max = slider.getMaximum();

        if (min < 0.0 && max > 0.0 && std::abs (min + max) < 1.0e-9)
            return slider.valueToProportionOfLength (0.0);

        return 0.0;
    }

    // A slot cut into the panel: dark inside, a shadow along the top (or
    // left) edge where the panel overhangs it, a catch of light along the
    // bottom (or right) one.
    static void drawSlot (juce::Graphics& g, juce::Rectangle<float> slot, bool vertical, float alpha)
    {
        const auto corner = juce::jmin (slot.getWidth(), slot.getHeight()) * 0.5f;

        g.setColour (juce::Colour (0xff111114).withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (slot, corner);

        g.setColour (juce::Colours::black.withAlpha (0.55f * alpha));
        g.drawRoundedRectangle (slot, corner, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.07f * alpha));
        if (vertical)
            g.drawVerticalLine ((int) slot.getRight(), slot.getY() + corner, slot.getBottom() - corner);
        else
            g.drawHorizontalLine ((int) slot.getBottom(), slot.getX() + corner, slot.getRight() - corner);
    }

    // A cap, lit from above like the knob caps: a soft shadow under it, a body
    // from a light top edge to a dark bottom one, and a face dished either side
    // of the line that marks the value -- top half darkening downwards, bottom
    // half lightening, the way a concave cap catches a light overhead.
    static void drawCap (juce::Graphics& g, juce::Rectangle<float> cap, bool vertical, float alpha)
    {
        const auto corner = juce::jmin (3.0f, juce::jmin (cap.getWidth(), cap.getHeight()) * 0.25f);

        g.setColour (juce::Colours::black.withAlpha (0.45f * alpha));
        g.fillRoundedRectangle (cap.translated (0.0f, 2.0f).expanded (1.0f), corner + 1.0f);

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff74747d).withMultipliedAlpha (alpha), 0.0f, cap.getY(),
                                                 juce::Colour (0xff1e1e22).withMultipliedAlpha (alpha), 0.0f, cap.getBottom(),
                                                 false));
        g.fillRoundedRectangle (cap, corner);

        const auto face = cap.reduced (juce::jmax (1.5f, juce::jmin (cap.getWidth(), cap.getHeight()) * 0.1f));

        if (vertical)
        {
            auto upper = face, lower = face;
            upper = upper.removeFromTop (face.getHeight() * 0.5f);
            lower = lower.removeFromBottom (face.getHeight() * 0.5f);

            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4c4c55).withMultipliedAlpha (alpha), 0.0f, upper.getY(),
                                                     juce::Colour (0xff2a2a30).withMultipliedAlpha (alpha), 0.0f, upper.getBottom(),
                                                     false));
            g.fillRect (upper);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff26262b).withMultipliedAlpha (alpha), 0.0f, lower.getY(),
                                                     juce::Colour (0xff3c3c44).withMultipliedAlpha (alpha), 0.0f, lower.getBottom(),
                                                     false));
            g.fillRect (lower);

            // Grip ridges across the face, a dark groove with light under it.
            const auto ridgeStep = face.getHeight() / 7.0f;

            for (int i : { -2, -1, 1, 2 })
            {
                const auto ry = face.getCentreY() + (float) i * ridgeStep;
                g.setColour (juce::Colours::black.withAlpha (0.45f * alpha));
                g.drawHorizontalLine ((int) ry, face.getX() + 1.0f, face.getRight() - 1.0f);
                g.setColour (juce::Colours::white.withAlpha (0.08f * alpha));
                g.drawHorizontalLine ((int) ry + 1, face.getX() + 1.0f, face.getRight() - 1.0f);
            }
        }
        else
        {
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3e3e46).withMultipliedAlpha (alpha), 0.0f, face.getY(),
                                                     juce::Colour (0xff2a2a30).withMultipliedAlpha (alpha), 0.0f, face.getBottom(),
                                                     false));
            g.fillRect (face);
        }

        g.setColour (juce::Colours::black.withAlpha (0.6f * alpha));
        g.drawRoundedRectangle (cap, corner, 1.0f);

        // The line that is the reading: white with a dark edge under it, as
        // on the knob's pointer.
        const auto lineColour = juce::Colour (0xfff0f0f4).withMultipliedAlpha (alpha);

        if (vertical)
        {
            const auto cy = cap.getCentreY();
            g.setColour (juce::Colours::black.withAlpha (0.5f * alpha));
            g.fillRect (juce::Rectangle<float> (face.getX(), cy - 1.5f, face.getWidth(), 3.0f));
            g.setColour (lineColour);
            g.fillRect (juce::Rectangle<float> (face.getX(), cy - 1.0f, face.getWidth(), 2.0f));
        }
        else
        {
            const auto cx = cap.getCentreX();
            g.setColour (juce::Colours::black.withAlpha (0.5f * alpha));
            g.fillRect (juce::Rectangle<float> (cx - 1.5f, face.getY(), 3.0f, face.getHeight()));
            g.setColour (lineColour);
            g.fillRect (juce::Rectangle<float> (cx - 1.0f, face.getY(), 2.0f, face.getHeight()));
        }
    }

    void drawFader (juce::Graphics& g, juce::Rectangle<float> bounds, float sliderPos,
                    juce::Slider& slider, float alpha)
    {
        const auto travelTop = bounds.getY() + (float) faderCapHeight * 0.5f;
        const auto travelBottom = bounds.getBottom() - (float) faderCapHeight * 0.5f;
        const auto travel = juce::jmax (1.0f, travelBottom - travelTop);
        const auto yFor = [&] (double proportion) { return travelBottom - (float) proportion * travel; };

        const auto cx = bounds.getCentreX();
        const auto capWidth = juce::jmin (bounds.getWidth() - 4.0f, 30.0f);

        // The scale, printed on the panel either side of the slot: a mark
        // at each of these, the 0 dB one brighter and longer. Only a fader in
        // tracktion's position space has decibels to mark.
        const bool isVolumeFader = std::abs (slider.getMinimum()) < 1.0e-9
                                   && std::abs (slider.getMaximum() - 1.0) < 1.0e-9;

        if (isVolumeFader)
        {
            const auto tickReach = capWidth * 0.5f + 3.0f;
            const auto labelFont = juce::Font (uiFont (fonts::small));
            g.setFont (labelFont);
            auto lastLabelY = std::numeric_limits<float>::lowest();

            for (const auto db : { 6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -48.0f })
            {
                const auto ty = yFor ((double) te::decibelsToVolumeFaderPosition (db));
                const bool isUnity = std::abs (db) < 0.01f;
                const auto len = isUnity ? 6.0f : 4.0f;

                g.setColour (juce::Colour (isUnity ? 0xff8a8a94 : 0xff55555d).withMultipliedAlpha (alpha));
                g.drawLine (cx - tickReach - len, ty, cx - tickReach, ty, 1.0f);
                g.drawLine (cx + tickReach, ty, cx + tickReach + len, ty, 1.0f);

                // Numbers to the left, where there is room for them, and only
                // where they do not run into the one above.
                const auto labelRight = cx - tickReach - len - 2.0f;
                const auto labelHeight = labelFont.getHeight();

                if (labelRight - bounds.getX() >= 16.0f && ty - lastLabelY >= labelHeight)
                {
                    g.setColour (juce::Colour (isUnity ? 0xff9a9aa4 : 0xff6a6a72).withMultipliedAlpha (alpha));
                    g.drawText (db > 0.0f ? "+" + juce::String ((int) db) : juce::String ((int) db),
                                juce::Rectangle<float> (bounds.getX(), ty - labelHeight * 0.5f,
                                                        labelRight - bounds.getX(), labelHeight),
                                juce::Justification::centredRight, false);
                    lastLabelY = ty;
                }
            }
        }

        // The slot, running the length of the travel and a little past it.
        const auto slot = juce::Rectangle<float> (cx - 2.5f, travelTop - 4.0f, 5.0f, travel + 8.0f);
        drawSlot (g, slot, true, alpha);

        // The glow from the zero point to the cap.
        const auto zeroY = yFor (zeroProportion (slider));

        if (std::abs (sliderPos - zeroY) > 0.5f)
        {
            g.setColour (juce::Colour (accentColour).withMultipliedAlpha (0.85f * alpha));
            g.fillRect (juce::Rectangle<float> (cx - 1.0f, juce::jmin (zeroY, sliderPos),
                                                2.0f, std::abs (sliderPos - zeroY)));
        }

        drawCap (g, juce::Rectangle<float> (capWidth, (float) faderCapHeight).withCentre ({ cx, sliderPos }),
                 true, alpha);
    }

    void drawSmallSlider (juce::Graphics& g, juce::Rectangle<float> bounds, float sliderPos,
                          juce::Slider& slider, float alpha)
    {
        const auto travelLeft = bounds.getX() + (float) panCapWidth * 0.5f;
        const auto travelRight = bounds.getRight() - (float) panCapWidth * 0.5f;
        const auto cy = bounds.getCentreY();

        const auto zeroProp = zeroProportion (slider);
        const auto zeroX = travelLeft + (float) zeroProp * (travelRight - travelLeft);

        // A bipolar slider has its centre printed above and below the slot.
        if (zeroProp > 0.0)
        {
            g.setColour (juce::Colour (0xff6a6a72).withMultipliedAlpha (alpha));
            g.drawVerticalLine ((int) zeroX, bounds.getY(), cy - 3.0f);
            g.drawVerticalLine ((int) zeroX, cy + 3.0f, bounds.getBottom());
        }

        drawSlot (g, juce::Rectangle<float> (travelLeft - 3.0f, cy - 2.0f, travelRight - travelLeft + 6.0f, 4.0f),
                  false, alpha);

        if (std::abs (sliderPos - zeroX) > 0.5f)
        {
            g.setColour (juce::Colour (accentColour).withMultipliedAlpha (0.85f * alpha));
            g.fillRect (juce::Rectangle<float> (juce::jmin (zeroX, sliderPos), cy - 1.0f,
                                                std::abs (sliderPos - zeroX), 2.0f));
        }

        const auto capHeight = juce::jmax (8.0f, bounds.getHeight() - 3.0f);
        drawCap (g, juce::Rectangle<float> ((float) panCapWidth, capHeight).withCentre ({ sliderPos, cy - 0.5f }),
                 false, alpha);
    }
};

} // namespace carve::app
