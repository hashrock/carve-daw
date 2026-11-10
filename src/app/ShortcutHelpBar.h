#pragma once

#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "TouchUi.h"

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
        if (isTouchUi)
            newEntries = forTouch (std::move (newEntries));

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
    // The entries are written for a mouse and a keyboard. On a touch screen
    // a click is a tap and a drag is still a drag, and those are reworded;
    // anything that needs a key or a second mouse button has no gesture to
    // stand in for it yet, and a bar that advertised it would be wrong.
    static std::vector<Entry> forTouch (std::vector<Entry> all)
    {
        std::vector<Entry> kept;

        for (auto& entry : all)
        {
            const auto keys = entry.keys.toLowerCase();
            const bool needsDesktop = keys.contains ("right") || keys.contains ("cmd") || keys.contains ("shift")
                                      || keys.contains ("alt") || keys.contains ("ctrl") || keys.contains ("scroll")
                                      || keys.contains ("hover") || keys.startsWith ("drop");

            // A key on its own: one character, or one of the named ones. What
            // is left after these is a gesture or the name of something on
            // screen ("+ FX", "dot").
            const bool isKey = keys.length() == 1
                               || juce::StringArray { "space", "backspace", "delete", "esc", "enter", "return", "tab" }
                                      .contains (keys);

            if (needsDesktop || isKey)
                continue;

            entry.keys = entry.keys.replace ("double click", "double tap", true)
                                   .replace ("double-click", "double tap", true)
                                   .replace ("dbl-click", "double tap", true)
                                   .replace ("click", "tap", true);
            entry.action = entry.action.replace ("double-click", "double tap", true)
                                       .replace ("click", "tap", true);
            kept.push_back (std::move (entry));
        }

        return kept;
    }

    std::vector<Entry> entries;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShortcutHelpBar)
};

} // namespace carve::app
