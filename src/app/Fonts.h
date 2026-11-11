#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace carve::app
{

// The sizes the app draws text at, and -- the reason this header exists -- the
// floor under all of them.
//
// Every size used to be picked where it was drawn: 9pt for a graph's axis,
// 10pt for the piano roll's octave marks, 10.5pt for a meter's scale. Each was
// reasonable next to the pixels around it, and the app as a whole came out
// unreadable, because "as small as this box allows" applied twenty times is a
// design nobody chose. So a size is asked for through uiFont(), which will not
// go under the floor, and a box too small for the floor is the box that has to
// give.
namespace fonts
{
    // 13, not 12. Most of the app was already drawing at 12, so a floor there
    // would have raised a handful of axis labels by a pixel and left every
    // name, value and readout exactly as it was -- a change that measures but
    // does not show. The floor has to sit above where the app already was, or
    // it is not raising anything.
    constexpr float minimumHeight = 13.0f;

    // What the app actually distinguishes. Anything else is a component with a
    // reason, and it still gets the floor.
    constexpr float small = 13.0f;     // secondary text: units, hints, axis labels
    constexpr float normal = 14.0f;    // most text: names, values, readouts
    constexpr float title = 16.0f;     // the heading of a panel or a window
} // namespace fonts

// A font at this height or the minimum, whichever is larger.
inline juce::FontOptions uiFont (float height, int styleFlags = juce::Font::plain)
{
    return juce::FontOptions (juce::jmax (fonts::minimumHeight, height), styleFlags);
}

inline juce::Font atLeastMinimumHeight (juce::Font font)
{
    return font.getHeight() < fonts::minimumHeight ? font.withHeight (fonts::minimumHeight) : font;
}

//==============================================================================
// The other half of the floor: the text this app does not paint itself.
//
// A label, a combo box, a menu or a button asks its LookAndFeel how to draw
// its text, and JUCE's answer is derived from the widget's height -- so a
// short button gets small text no matter what this app would prefer. Clamping
// the handful of getters that return a font is what makes the minimum hold
// everywhere rather than only where uiFont() is called.
//
// Nothing else here is styled: the rest is JUCE's default look, which is what
// the app was written against.
class CarveLookAndFeel : public juce::LookAndFeel_V4
{
public:
    juce::Font getLabelFont (juce::Label& label) override
    {
        return atLeastMinimumHeight (juce::LookAndFeel_V4::getLabelFont (label));
    }

    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        return atLeastMinimumHeight (juce::LookAndFeel_V4::getComboBoxFont (box));
    }

    juce::Font getPopupMenuFont() override
    {
        return atLeastMinimumHeight (juce::LookAndFeel_V4::getPopupMenuFont());
    }

    juce::Font getTextButtonFont (juce::TextButton& button, int buttonHeight) override
    {
        return atLeastMinimumHeight (juce::LookAndFeel_V4::getTextButtonFont (button, buttonHeight));
    }

    juce::Font getSliderPopupFont (juce::Slider& slider) override
    {
        return atLeastMinimumHeight (juce::LookAndFeel_V4::getSliderPopupFont (slider));
    }

    juce::Font getTabButtonFont (juce::TabBarButton& button, float height) override
    {
        return atLeastMinimumHeight (juce::LookAndFeel_V4::getTabButtonFont (button, height));
    }

    juce::Font getAlertWindowMessageFont() override
    {
        return atLeastMinimumHeight (juce::LookAndFeel_V4::getAlertWindowMessageFont());
    }

    juce::Font getAlertWindowTitleFont() override
    {
        return atLeastMinimumHeight (juce::LookAndFeel_V4::getAlertWindowTitleFont());
    }

    juce::Font getAlertWindowFont() override
    {
        return atLeastMinimumHeight (juce::LookAndFeel_V4::getAlertWindowFont());
    }

    // The close button of a window that draws its own title bar, which today
    // is every secondary window on the iPad. JUCE's is a dark red cross on a
    // dark bar, hard to find and harder to hit; this one is a bright cross
    // that fills red under the finger.
    juce::Button* createDocumentWindowButton (int buttonType) override
    {
        if (buttonType == juce::DocumentWindow::closeButton)
            return new CloseButton();

        return juce::LookAndFeel_V4::createDocumentWindowButton (buttonType);
    }

private:
    struct CloseButton : juce::Button
    {
        CloseButton() : juce::Button ("close") {}

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto red = juce::Colour (0xffff5f57);
            const auto bounds = getLocalBounds().toFloat();

            if (highlighted || down)
            {
                g.setColour (down ? red.darker (0.2f) : red);
                g.fillRect (bounds);
            }

            const auto side = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.38f;
            const auto cross = bounds.withSizeKeepingCentre (side, side);

            g.setColour (! isEnabled() ? juce::Colours::grey
                                       : (highlighted || down) ? juce::Colours::white : red);
            g.drawLine ({ cross.getTopLeft(), cross.getBottomRight() }, 2.2f);
            g.drawLine ({ cross.getTopRight(), cross.getBottomLeft() }, 2.2f);
        }
    };
};

} // namespace carve::app
