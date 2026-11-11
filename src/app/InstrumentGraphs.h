#pragma once

#include <functional>
#include <vector>

#include "Fonts.h"

namespace carve::app::graphs
{

// What the recorded instruments' editors (Acoustic Drums, Electric Bass) draw
// with: a plot that several related knobs are handles on. One drawing per
// group of knobs that do one thing to the sound -- the attack, body and noise
// of a drum are three heights in one envelope, a bass's pluck position and
// palm mute are two places on one string -- so that what a knob does is the
// picture moving, not a number changing.

namespace colours
{
    constexpr juce::uint32 background = 0xff232327, panel = 0xff2a2a31, plot = 0xff1d1d22,
                           grid = 0xff34343d, text = 0xfff0f0f4, dimText = 0xffb8b8c0, faint = 0xff6c6c78,
                           accent = 0xffe0a24f,     // the app's orange
                           violet = 0xff9a86f0, teal = 0xff5fc4b8, rose = 0xffe07a8f;
} // namespace colours

// A titled box the graphs and knobs sit in.
inline void drawPanel (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    g.setColour (juce::Colour (colours::panel));
    g.fillRoundedRectangle (area.toFloat(), 4.0f);

    if (title.isNotEmpty())
    {
        g.setColour (juce::Colour (colours::dimText));
        g.setFont (uiFont (fonts::small, juce::Font::bold));
        g.drawText (title.toUpperCase(), area.reduced (8, 0).removeFromTop (20), juce::Justification::centredLeft);
    }
}

constexpr int panelTitleHeight = 20;

//==============================================================================
// A plot with handles. A handle is a point on the drawing that a drag moves;
// what the move means is the subclass's (a dB, a time, a position along a
// string). Hovering a handle names it and gives its value; double-clicking
// puts it back to its default.
class HandleGraph : public juce::Component,
                    public juce::SettableTooltipClient
{
public:
    struct Handle
    {
        juce::Point<float> position;   // in component coordinates
        juce::Colour colour;
        juce::String name;
        juce::String value;
        bool hollow = false;           // drawn as a ring: at its "as recorded" setting
    };

    // While any handle is held, so an owner can hold off expensive redraws
    // until the drag ends if it needs to.
    bool isDragging() const  { return active >= 0; }

    void paint (juce::Graphics& g) override
    {
        const auto plot = getPlotArea();

        g.setColour (juce::Colour (colours::plot));
        g.fillRoundedRectangle (plot, 3.0f);

        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (plot.toNearestInt());
            paintPlot (g, plot);
        }

        paintOverlay (g, plot);

        handles = getHandles();

        for (int i = 0; i < (int) handles.size(); ++i)
        {
            const auto& h = handles[(size_t) i];
            const bool lit = i == active || (active < 0 && i == hovered);
            const auto r = juce::Rectangle<float> (lit ? 13.0f : 11.0f, lit ? 13.0f : 11.0f).withCentre (h.position);

            g.setColour (juce::Colour (colours::plot));
            g.fillEllipse (r.expanded (2.0f));
            g.setColour (h.colour);

            if (h.hollow)
                g.drawEllipse (r.reduced (1.0f), 2.0f);
            else
                g.fillEllipse (r);
        }

        // The held or hovered handle's name and value, in a tag beside it.
        const int shown = active >= 0 ? active : hovered;

        if (juce::isPositiveAndBelow (shown, (int) handles.size()))
        {
            const auto& h = handles[(size_t) shown];
            const auto text = h.value.isNotEmpty() ? h.name + "  " + h.value : h.name;
            const auto font = juce::Font (uiFont (fonts::small, juce::Font::bold));
            const auto w = (float) juce::GlyphArrangement::getStringWidthInt (font, text) + 12.0f;
            auto tag = juce::Rectangle<float> (w, 18.0f).withCentre (h.position.translated (0.0f, h.position.y > plot.getY() + 30.0f ? -20.0f : 20.0f));
            tag.setX (juce::jlimit (plot.getX(), juce::jmax (plot.getX(), plot.getRight() - w), tag.getX()));

            g.setColour (juce::Colour (colours::background).withAlpha (0.92f));
            g.fillRoundedRectangle (tag, 3.0f);
            g.setColour (h.colour);
            g.drawRoundedRectangle (tag, 3.0f, 1.0f);
            g.setColour (juce::Colour (colours::text));
            g.setFont (font);
            g.drawText (text, tag, juce::Justification::centred);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = findHandle (e.position);

        if (h != hovered)
        {
            hovered = h;
            setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hovered >= 0)
        {
            hovered = -1;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        active = findHandle (e.position);

        if (active < 0)
            active = handleForEmptyClick (e.position);

        if (active >= 0)
        {
            dragStart = e.position;
            beginDrag (active);

            // A click on empty plot that picked a handle moves it there at once.
            if (findHandle (e.position) < 0)
                dragHandle (active, e.position, {});
        }

        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (active < 0)
            return;

        dragHandle (active, e.position, e.position - dragStart);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (active >= 0)
            endDrag (active);

        active = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int h = findHandle (e.position);

        if (h >= 0)
        {
            resetHandle (h);
            repaint();
        }
    }

protected:
    virtual juce::Rectangle<float> getPlotArea() const  { return getLocalBounds().toFloat(); }
    virtual void paintPlot (juce::Graphics&, juce::Rectangle<float> plot) = 0;
    virtual void paintOverlay (juce::Graphics&, juce::Rectangle<float>) {}
    virtual std::vector<Handle> getHandles() = 0;

    virtual void beginDrag (int) {}
    virtual void dragHandle (int handle, juce::Point<float> position, juce::Point<float> delta) = 0;
    virtual void endDrag (int) {}
    virtual void resetHandle (int) {}

    // Which handle a click on the plot away from every handle takes, -1 for
    // none. For a plot that is all one control (an XY pad).
    virtual int handleForEmptyClick (juce::Point<float>)  { return -1; }

private:
    int findHandle (juce::Point<float> p)
    {
        handles = getHandles();
        int best = -1;
        float bestDistance = 12.0f;

        for (int i = 0; i < (int) handles.size(); ++i)
            if (const auto d = handles[(size_t) i].position.getDistanceFrom (p); d < bestDistance)
            {
                bestDistance = d;
                best = i;
            }

        return best;
    }

    std::vector<Handle> handles;
    int active = -1, hovered = -1;
    juce::Point<float> dragStart;
};

//==============================================================================
// A dashed horizontal or vertical guide with a label, for "0 dB", "200 Hz".
inline void drawGuide (juce::Graphics& g, juce::Line<float> line, const juce::String& label = {})
{
    g.setColour (juce::Colour (colours::grid));
    const float dashes[] { 3.0f, 3.0f };
    g.drawDashedLine (line, dashes, 2, 1.0f);

    if (label.isNotEmpty())
    {
        g.setColour (juce::Colour (colours::faint));
        g.setFont (uiFont (fonts::small));
        g.drawText (label, juce::Rectangle<float> (60.0f, 14.0f).withPosition (line.getStart().translated (3.0f, 1.0f)),
                    juce::Justification::topLeft);
    }
}

inline juce::String signedValue (double v, int decimals, const juce::String& unit)
{
    return (v > 0.0 ? "+" : "") + juce::String (v, decimals) + unit;
}

} // namespace carve::app::graphs
