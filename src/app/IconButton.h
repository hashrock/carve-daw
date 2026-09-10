#pragma once

#include <cmath>

#include <juce_gui_basics/juce_gui_basics.h>

namespace carve::app
{

// The symbols the app's buttons carry. Drawn from geometry rather than loaded
// from files, so they take the button's text colour, scale with it, and cost
// nothing to add to a new button.
enum class Icon
{
    play, pause, stop, loop, rewind, fastForward,
    mixer, exportFile, undo, redo, save, open,
    instrument, pianoRoll,
    plus, clone, menu,
    pencil, select,
    zoomIn, zoomOut,
    folder, audioFile, star, arrowUp, speaker
};

namespace icons
{
    // Every shape is drawn in a unit square and scaled to `bounds` at the end,
    // so the proportions below are the whole design.
    namespace detail
    {
        inline juce::Path stroked (const juce::Path& centreLine, float width)
        {
            juce::Path out;
            juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
                .createStrokedPath (out, centreLine);
            return out;
        }

        // Degrees clockwise from twelve o'clock, either direction, as a
        // polyline: Path::addCentredArc is fine but this keeps the end points
        // in hand for the arrowheads.
        inline juce::Path arc (juce::Point<float> centre, float radius, float fromDeg, float toDeg,
                               juce::Point<float>* endPoint = nullptr,
                               juce::Point<float>* endDirection = nullptr)
        {
            juce::Path p;
            constexpr int steps = 28;
            juce::Point<float> previous;

            for (int i = 0; i <= steps; ++i)
            {
                const auto a = juce::degreesToRadians (fromDeg + (toDeg - fromDeg) * (float) i / (float) steps);
                const juce::Point<float> pt (centre.x + radius * std::sin (a), centre.y - radius * std::cos (a));

                if (i == 0)
                    p.startNewSubPath (pt);
                else
                    p.lineTo (pt);

                if (i == steps)
                {
                    if (endPoint != nullptr)
                        *endPoint = pt;

                    if (endDirection != nullptr)
                        *endDirection = (pt - previous) / (pt - previous).getDistanceFromOrigin();
                }

                previous = pt;
            }

            return p;
        }

        inline void addArrowHead (juce::Path& into, juce::Point<float> at, juce::Point<float> dir, float size)
        {
            const juce::Point<float> n (-dir.y, dir.x);
            const auto tip = at + dir * (size * 0.6f);
            const auto base = at - dir * (size * 0.4f);
            into.addTriangle (tip, base + n * (size * 0.5f), base - n * (size * 0.5f));
        }
    } // namespace detail

    inline juce::Path make (Icon icon, juce::Rectangle<float> bounds)
    {
        using namespace detail;
        juce::Path p;

        switch (icon)
        {
            case Icon::play:
                p.addTriangle (0.08f, 0.0f, 0.08f, 1.0f, 0.98f, 0.5f);
                break;

            case Icon::pause:
                p.addRectangle (0.06f, 0.0f, 0.32f, 1.0f);
                p.addRectangle (0.62f, 0.0f, 0.32f, 1.0f);
                break;

            case Icon::stop:
                p.addRectangle (0.08f, 0.08f, 0.84f, 0.84f);
                break;

            case Icon::fastForward:
            case Icon::rewind:
                // Two triangles nose to tail; rewind is the mirror image.
                p.addTriangle (0.0f, 0.12f, 0.0f, 0.88f, 0.5f, 0.5f);
                p.addTriangle (0.5f, 0.12f, 0.5f, 0.88f, 1.0f, 0.5f);
                if (icon == Icon::rewind)
                    p.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f).translated (1.0f, 0.0f));
                break;

            case Icon::loop:
            {
                juce::Point<float> end, dir;
                p = stroked (arc ({ 0.5f, 0.5f }, 0.36f, 45.0f, 335.0f, &end, &dir), 0.14f);
                addArrowHead (p, end, dir, 0.42f);
                break;
            }

            case Icon::mixer:
            {
                // Three faders: the lines are the slots, the blocks the caps.
                juce::Path slots;
                for (float x : { 0.18f, 0.5f, 0.82f })
                {
                    slots.startNewSubPath (x, 0.06f);
                    slots.lineTo (x, 0.94f);
                }
                p = stroked (slots, 0.1f);
                p.addRoundedRectangle (0.02f, 0.5f, 0.32f, 0.2f, 0.04f);
                p.addRoundedRectangle (0.34f, 0.22f, 0.32f, 0.2f, 0.04f);
                p.addRoundedRectangle (0.66f, 0.6f, 0.32f, 0.2f, 0.04f);
                break;
            }

            case Icon::exportFile:
            {
                // An arrow up and out of a tray.
                juce::Path tray;
                tray.startNewSubPath (0.1f, 0.56f);
                tray.lineTo (0.1f, 0.94f);
                tray.lineTo (0.9f, 0.94f);
                tray.lineTo (0.9f, 0.56f);
                p = stroked (tray, 0.13f);
                p.addRectangle (0.42f, 0.24f, 0.16f, 0.46f);
                p.addTriangle (0.5f, 0.0f, 0.2f, 0.34f, 0.8f, 0.34f);
                break;
            }

            case Icon::undo:
            case Icon::redo:
            {
                // A curl over the top, ending on the left with the head
                // pointing down; redo is the same thing mirrored.
                juce::Point<float> end, dir;
                p = stroked (arc ({ 0.5f, 0.54f }, 0.34f, 118.0f, -98.0f, &end, &dir), 0.14f);
                addArrowHead (p, end, dir, 0.44f);

                if (icon == Icon::redo)
                    p.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f).translated (1.0f, 0.0f));
                break;
            }

            case Icon::save:
                // The floppy, with the label slot and the shutter cut out of
                // it -- even-odd winding makes the inner rectangles holes.
                p.addRoundedRectangle (0.04f, 0.04f, 0.92f, 0.92f, 0.12f);
                p.addRectangle (0.26f, 0.04f, 0.42f, 0.28f);
                p.addRectangle (0.22f, 0.58f, 0.56f, 0.38f);
                p.setUsingNonZeroWinding (false);
                break;

            case Icon::open:
                // A folder, tab and all, as one silhouette.
                p.startNewSubPath (0.04f, 0.14f);
                p.lineTo (0.4f, 0.14f);
                p.lineTo (0.5f, 0.28f);
                p.lineTo (0.96f, 0.28f);
                p.lineTo (0.96f, 0.86f);
                p.lineTo (0.04f, 0.86f);
                p.closeSubPath();
                break;

            case Icon::instrument:
            {
                // One cycle of a sine: the signal a synth makes.
                juce::Path wave;
                constexpr int steps = 32;
                for (int i = 0; i <= steps; ++i)
                {
                    const auto x = (float) i / (float) steps;
                    const auto y = 0.5f - 0.34f * std::sin (x * juce::MathConstants<float>::twoPi);
                    if (i == 0) wave.startNewSubPath (0.02f + x * 0.96f, y);
                    else        wave.lineTo (0.02f + x * 0.96f, y);
                }
                p = stroked (wave, 0.14f);
                break;
            }

            case Icon::pianoRoll:
            {
                // A stretch of keyboard: the frame, and three black keys.
                juce::Path frame;
                frame.addRectangle (0.06f, 0.1f, 0.88f, 0.8f);
                p = stroked (frame, 0.1f);
                for (float x : { 0.22f, 0.44f, 0.66f })
                    p.addRectangle (x, 0.1f, 0.12f, 0.44f);
                break;
            }

            case Icon::plus:
                p.addRectangle (0.42f, 0.04f, 0.16f, 0.92f);
                p.addRectangle (0.04f, 0.42f, 0.92f, 0.16f);
                break;

            case Icon::clone:
            {
                // Two cards: the one behind is an outline, the one in front is
                // solid, which is what says "and now there are two".
                juce::Path back;
                back.addRectangle (0.34f, 0.04f, 0.62f, 0.62f);
                p = stroked (back, 0.1f);
                p.addRectangle (0.04f, 0.34f, 0.62f, 0.62f);
                break;
            }

            case Icon::menu:
                for (float x : { 0.16f, 0.5f, 0.84f })
                    p.addEllipse (x - 0.11f, 0.39f, 0.22f, 0.22f);
                break;

            case Icon::pencil:
                // Built lying flat, then tipped to point down-left.
                p.addRectangle (0.16f, 0.39f, 0.58f, 0.22f);       // body
                p.addTriangle (0.16f, 0.39f, 0.16f, 0.61f, 0.0f, 0.5f);   // tip
                p.addRectangle (0.8f, 0.39f, 0.16f, 0.22f);        // cap
                p.applyTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::pi / 4.0f, 0.5f, 0.5f));
                break;

            case Icon::select:
                // The pointer itself.
                p.startNewSubPath (0.18f, 0.04f);
                p.lineTo (0.18f, 0.8f);
                p.lineTo (0.38f, 0.62f);
                p.lineTo (0.52f, 0.96f);
                p.lineTo (0.66f, 0.9f);
                p.lineTo (0.52f, 0.58f);
                p.lineTo (0.78f, 0.58f);
                p.closeSubPath();
                break;

            case Icon::folder:
                // The same silhouette as `open`, kept as its own name so the
                // browser's rows and the file menu can part ways later.
                p = make (Icon::open, { 0.0f, 0.0f, 1.0f, 1.0f });
                break;

            case Icon::audioFile:
                // A handful of waveform bars: the shape a sample has in every
                // editor, at a size where a real waveform would be noise.
                for (auto [x, h] : { std::pair { 0.08f, 0.36f }, std::pair { 0.3f, 0.86f },
                                     std::pair { 0.52f, 0.6f }, std::pair { 0.74f, 0.96f } })
                    p.addRoundedRectangle (x, 0.5f - h * 0.5f, 0.18f, h, 0.05f);
                break;

            case Icon::star:
            {
                // Five points, the outer radius on the unit square's edge.
                constexpr float outer = 0.5f, inner = 0.2f;
                for (int i = 0; i < 10; ++i)
                {
                    const auto a = juce::MathConstants<float>::pi * (float) i / 5.0f;
                    const auto r = (i % 2 == 0) ? outer : inner;
                    const juce::Point<float> pt (0.5f + r * std::sin (a), 0.52f - r * std::cos (a));
                    if (i == 0) p.startNewSubPath (pt);
                    else        p.lineTo (pt);
                }
                p.closeSubPath();
                break;
            }

            case Icon::arrowUp:
                // Out of the folder and up a level: a stem with a head on top.
                p.addRectangle (0.42f, 0.36f, 0.16f, 0.6f);
                p.addTriangle (0.5f, 0.02f, 0.14f, 0.44f, 0.86f, 0.44f);
                break;

            case Icon::speaker:
            {
                // A cone, and one sound wave in front of it.
                p.addRectangle (0.04f, 0.34f, 0.2f, 0.32f);
                p.addQuadrilateral (0.2f, 0.34f, 0.5f, 0.08f, 0.5f, 0.92f, 0.2f, 0.66f);
                p.addPath (stroked (arc ({ 0.5f, 0.5f }, 0.34f, 40.0f, 140.0f), 0.12f));
                break;
            }

            case Icon::zoomIn:
            case Icon::zoomOut:
            {
                // A magnifier: the lens is a stroked circle, so it stays a ring
                // without needing even-odd, and the handle starts at its rim.
                juce::Path lens;
                lens.addEllipse (0.06f, 0.06f, 0.66f, 0.66f);
                p = stroked (lens, 0.12f);

                juce::Path handle;
                handle.startNewSubPath (0.62f, 0.62f);
                handle.lineTo (0.94f, 0.94f);
                p.addPath (stroked (handle, 0.16f));

                p.addRectangle (0.24f, 0.35f, 0.3f, 0.09f);
                if (icon == Icon::zoomIn)
                    p.addRectangle (0.345f, 0.245f, 0.09f, 0.3f);
                break;
            }
        }

        p.applyTransform (juce::AffineTransform::scale (bounds.getWidth(), bounds.getHeight())
                              .translated (bounds.getX(), bounds.getY()));
        return p;
    }
} // namespace icons

//==============================================================================
// A TextButton that carries a symbol beside its word, or a symbol alone when
// the word is empty. Buttons are found by shape long before they are read,
// which is what a row of same-looking word-buttons was missing.
//
// The background is still the look and feel's, so these sit among plain
// TextButtons without looking like a different kind of thing, and toggle
// colours, connected edges, radio groups and tooltips all work as before --
// only what is painted on top is ours.
class IconButton : public juce::TextButton
{
public:
    IconButton (const juce::String& text, Icon initialIcon)
        : juce::TextButton (text), icon (initialIcon)
    {
    }

    void setIcon (Icon newIcon)
    {
        if (icon == newIcon)
            return;

        icon = newIcon;
        repaint();
    }

    Icon getIcon() const  { return icon; }

    // No background at all: the symbol sits straight on the bar, and only
    // lights up under the pointer or when toggled on. For the controls that
    // are glanced at rather than read -- loop, undo, redo -- where a row of
    // bordered boxes was more frame than content.
    void setFlat (bool shouldBeFlat)
    {
        flat = shouldBeFlat;
        repaint();
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto& lf = getLookAndFeel();

        if (flat)
        {
            if (highlighted || down)
            {
                g.setColour (juce::Colours::white.withAlpha (down ? 0.16f : 0.08f));
                g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f);
            }
        }
        else
        {
            lf.drawButtonBackground (g, *this,
                                     findColour (getToggleState() ? buttonOnColourId : buttonColourId),
                                     highlighted, down);
        }

        const auto font = lf.getTextButtonFont (*this, getHeight());
        const auto text = getButtonText();
        const auto textWidth = text.isEmpty() ? 0.0f
                                              : (float) juce::GlyphArrangement::getStringWidthInt (font, text);

        // The symbol is sized from the button, so a toolbar button and a
        // transport button each get one in proportion.
        const auto iconSize = juce::jlimit (9.0f, 13.0f, (float) getHeight() * 0.42f);
        const auto gap = text.isEmpty() ? 0.0f : iconGap;

        // Icon and word are centred as one block, so a button whose word
        // changes -- Play to Pause -- does not shuffle its symbol about.
        const auto area = getLocalBounds().toFloat().reduced (5.0f, 0.0f);
        const auto blockWidth = iconSize + gap + textWidth;
        const auto x = area.getX() + juce::jmax (0.0f, (area.getWidth() - blockWidth) * 0.5f);

        g.setColour (findColour (getToggleState() ? textColourOnId : textColourOffId)
                         .withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f));

        g.fillPath (icons::make (icon, { x, area.getCentreY() - iconSize * 0.5f, iconSize, iconSize }));

        if (! text.isEmpty())
        {
            g.setFont (font);
            g.drawText (text, juce::Rectangle<float> (x + iconSize + gap, area.getY(),
                                                      textWidth, area.getHeight()),
                        juce::Justification::centredLeft, false);
        }
    }

private:
    static constexpr float iconGap = 5.0f;

    Icon icon;
    bool flat = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
};

//==============================================================================
// The strip of tools above a view that shows time running left to right. The
// playlist has one and the piano roll has one, and to anyone using them they
// are the same strip: the same pencil, the same rubber band, the same pair of
// zoom buttons.
//
// They were laid out separately, each with its own hand-written pixel widths,
// and came out 18px tall in the playlist against 24px in the piano roll --
// which IconButton then turned into two different icon sizes and two different
// font sizes, since it takes both from the button's height. Nobody chose that;
// it is what two sets of numbers do when they are written a month apart. So
// the numbers live here, once.
namespace toolStrip
{
    constexpr int height = 34;         // the strip itself
    constexpr int verticalPadding = 5; // above and below the buttons
    constexpr int buttonHeight = height - verticalPadding * 2;

    constexpr int toolWidth = 70;      // "Paint" / "Draw", "Select"
    constexpr int zoomWidth = 24;      // the two magnifiers, which carry no word
} // namespace toolStrip

} // namespace carve::app
