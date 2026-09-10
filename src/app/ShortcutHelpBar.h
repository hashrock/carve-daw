#pragma once

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"

namespace carve::app
{

// A strip along the bottom of a window listing the shortcuts that work *right
// now*. The owner re-sets the entries whenever that changes — a different tool,
// a different selection, a held modifier — so the bar always describes the
// state the user is actually in rather than everything the window can do.
class ShortcutHelpBar : public juce::Component
{
public:
    struct Entry
    {
        juce::String keys;     // as printed on the cap, e.g. "Cmd+D" or "drag"
        juce::String action;

        bool operator== (const Entry&) const = default;
    };

    static constexpr int preferredHeight = 24;

    ShortcutHelpBar()
    {
        setInterceptsMouseClicks (false, false);
    }

    void setEntries (std::vector<Entry> newEntries)
    {
        if (newEntries == entries)
            return;

        entries = std::move (newEntries);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1c1c20));
        g.setColour (juce::Colour (0xff3a3a40));
        g.drawHorizontalLine (0, 0.0f, (float) getWidth());

        auto area = getLocalBounds().reduced (8, 0);
        const auto capFont = uiFont (fonts::small);
        const auto textFont = uiFont (fonts::small);

        for (const auto& entry : entries)
        {
            const auto capWidth = juce::GlyphArrangement::getStringWidthInt (juce::Font (capFont),
                                                                             entry.keys) + 10;
            const auto textWidth = juce::GlyphArrangement::getStringWidthInt (juce::Font (textFont),
                                                                              entry.action) + 4;

            // Drop whatever no longer fits rather than crowding: the entries
            // are ordered most-useful-first by the caller.
            if (capWidth + textWidth + 12 > area.getWidth())
                break;

            auto cap = area.removeFromLeft (capWidth).reduced (0, 4);
            g.setColour (juce::Colour (0xff35353d));
            g.fillRoundedRectangle (cap.toFloat(), 3.0f);
            g.setColour (juce::Colour (0xffd8d8dc));
            g.setFont (capFont);
            g.drawText (entry.keys, cap, juce::Justification::centred);

            area.removeFromLeft (4);
            g.setColour (juce::Colour (0xff9a9aa4));
            g.setFont (textFont);
            g.drawText (entry.action, area.removeFromLeft (textWidth),
                        juce::Justification::centredLeft);

            area.removeFromLeft (12);
        }
    }

private:
    std::vector<Entry> entries;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShortcutHelpBar)
};

} // namespace carve::app
