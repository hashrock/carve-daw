#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/SongModel.h"

namespace carve::app::playmode
{

// The marks a break slicer's notes carry for how they play, drawn the same
// on the notes in the roll and on the buttons that set them, so the button
// that was pressed is the mark that appears.
inline void drawGlyph (juce::Graphics& g, model::Note::PlayMode mode, juce::Rectangle<float> box,
                       bool ramped, juce::Colour ink)
{
    using Mode = model::Note::PlayMode;
    g.setColour (ink);
    juce::Path path;

    switch (mode)
    {
        case Mode::forward:
            path.addTriangle (box.getX(), box.getY(), box.getRight(), box.getCentreY(), box.getX(), box.getBottom());
            g.fillPath (path);
            return;

        case Mode::reverse:
            path.addTriangle (box.getX(), box.getCentreY(), box.getRight(), box.getY(), box.getRight(), box.getBottom());
            g.fillPath (path);
            return;

        case Mode::tapeStop:
            // Coming down off speed: steep at first, flat at rest.
            path.startNewSubPath (box.getX(), box.getY());
            path.quadraticTo (box.getX() + box.getWidth() * 0.4f, box.getBottom(), box.getRight(), box.getBottom());
            break;

        case Mode::fadeIn:
        case Mode::fadeOut:
        {
            // The level over the note, as a fade is drawn on a clip.
            const bool in = mode == Mode::fadeIn;
            path.startNewSubPath (box.getX(), box.getBottom());
            path.lineTo (box.getRight(), in ? box.getY() : box.getBottom());
            path.lineTo (in ? box.getRight() : box.getX(), in ? box.getBottom() : box.getY());
            path.closeSubPath();
            g.fillPath (path);
            return;
        }

        case Mode::buzz:
        {
            // A saw: even teeth for a fixed buzz, tightening for a ramp.
            auto x = box.getX();
            path.startNewSubPath (x, box.getBottom());

            for (int i = 0; i < 4 && x < box.getRight() - 0.5f; ++i)
            {
                const auto width = juce::jmin (box.getRight() - x,
                                               box.getWidth() * (ramped ? 0.48f / (float) (i + 1) : 0.25f));
                path.lineTo (x + width, box.getY());
                path.lineTo (x + width, box.getBottom());
                x += width;
            }

            break;
        }
    }

    g.strokePath (path, juce::PathStrokeType (1.4f, juce::PathStrokeType::mitered, juce::PathStrokeType::rounded));
}

// A roll's dynamics as the hairpins a score writes them with.
inline void drawDynamicsGlyph (juce::Graphics& g, model::Note::RollDynamics dynamics, juce::Rectangle<float> box,
                               juce::Colour ink)
{
    using Dynamics = model::Note::RollDynamics;
    g.setColour (ink);
    juce::Path path;

    switch (dynamics)
    {
        case Dynamics::flat:
            path.startNewSubPath (box.getX(), box.getCentreY() - 2.0f);
            path.lineTo (box.getRight(), box.getCentreY() - 2.0f);
            path.startNewSubPath (box.getX(), box.getCentreY() + 2.0f);
            path.lineTo (box.getRight(), box.getCentreY() + 2.0f);
            break;

        case Dynamics::crescendo:
            path.startNewSubPath (box.getRight(), box.getY());
            path.lineTo (box.getX(), box.getCentreY());
            path.lineTo (box.getRight(), box.getBottom());
            break;

        case Dynamics::decrescendo:
            path.startNewSubPath (box.getX(), box.getY());
            path.lineTo (box.getRight(), box.getCentreY());
            path.lineTo (box.getX(), box.getBottom());
            break;
    }

    g.strokePath (path, juce::PathStrokeType (1.3f));
}

// A toggle button that shows a glyph where a TextButton shows its text, in
// the same colours and with the same connected edges, so a row of them
// reads as one segmented control alongside the text buttons around it.
class GlyphButton : public juce::Button
{
public:
    using Drawer = std::function<void (juce::Graphics&, juce::Rectangle<float>, juce::Colour)>;

    GlyphButton (const juce::String& name, Drawer drawerToUse)
        : juce::Button (name), drawer (std::move (drawerToUse))
    {
        setClickingTogglesState (false);
        setWantsKeyboardFocus (false);
        setTooltip (name);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const bool on = getToggleState();
        const auto background = findColour (on ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
        getLookAndFeel().drawButtonBackground (g, *this, background, highlighted, down);

        const auto ink = findColour (on ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                             .withMultipliedAlpha (isEnabled() ? 1.0f : 0.4f);
        const auto size = juce::jmin ((float) getHeight() * 0.42f, (float) getWidth() * 0.5f);

        if (drawer)
            drawer (g, getLocalBounds().toFloat().withSizeKeepingCentre (size * 1.25f, size), ink);
    }

private:
    Drawer drawer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlyphButton)
};

} // namespace carve::app::playmode
