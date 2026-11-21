#pragma once

#include <array>
#include <functional>

#include "Fonts.h"
#include "KnobPanel.h"
#include "PresetManager.h"
#include "plugins/DrumModelPlugin.h"

namespace carve::app
{

// The editor for the modelled kit: a section of knobs per drum (tuning,
// muffling, pitch glide, stick or beater hardness), the cymbals' tuning and
// levels, and the mics, room and volume; under them a strip of pads, one per
// note the kit answers to, that play it when clicked and light while their
// drum is still moving.
class DrumModelEditor : public juce::Component,
                        private juce::Timer
{
public:
    // Plays a note through the generator's track, the way the piano roll's
    // previews do. Wired by the generator window.
    std::function<void (int note, int velocity)> onPreviewNote;

    explicit DrumModelEditor (plugins::DrumModelPlugin& kitToEdit)
        : plugin (kitToEdit), presetBar (kitToEdit)
    {
        using P = plugins::DrumModelPlugin;
        using S = KnobSize;
        auto isAlive = [this] { return plugin != nullptr; };

        auto knob = [&] (P::Param p, const juce::String& name, KnobSize size = S::medium)
        {
            return std::make_unique<ParameterKnob> (kitToEdit.getParam (p), name, size, isAlive);
        };

        {
            auto& s = page.add (std::make_unique<KnobSection> ("Kick 22\""));
            s.addKnob (knob (P::kickTune, "Tune"));
            s.addKnob (knob (P::kickMute, "Muffle"));
            s.addKnob (knob (P::kickGlide, "Glide"));
            s.addKnob (knob (P::kickBeater, "Beater"));
            s.addKnob (knob (P::kickLevel, "Level"));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Snare 14\""));
            s.addHeaderControl (std::make_unique<PropertyToggleButton> (kitToEdit.state, P::getParamInfo (P::snareRim).id,
                                                                        "Rimshots", isAlive, false));
            s.addKnob (knob (P::snareTune, "Tune"));
            s.addKnob (knob (P::snareMute, "Muffle"));
            s.addKnob (knob (P::snareWires, "Wires"));
            s.addKnob (knob (P::snareStick, "Stick"));
            s.addKnob (knob (P::snareLevel, "Level"));
        }
        page.newRow();

        const char* tomNames[] { "Tom 10\"", "Tom 12\"", "Floor Tom 16\"" };
        for (int t = 0; t < 3; ++t)
        {
            const int base = P::tom1Tune + 4 * t;
            auto& s = page.add (std::make_unique<KnobSection> (tomNames[t]));
            s.addKnob (knob ((P::Param) base, "Tune"));
            s.addKnob (knob ((P::Param) (base + 1), "Muffle"));
            s.addKnob (knob ((P::Param) (base + 2), "Glide"));
            s.addKnob (knob ((P::Param) (base + 3), "Stick"));
        }
        page.newRow();
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Cymbals"));
            s.addKnob (knob (P::hatTune, "Hat Tune"));
            s.addKnob (knob (P::hatLevel, "Hat"));
            s.addKnob (knob (P::crashTune, "Crash Tune"));
            s.addKnob (knob (P::crashLevel, "Crash"));
            s.addKnob (knob (P::rideTune, "Ride Tune"));
            s.addKnob (knob (P::rideLevel, "Ride"));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Mix"));
            s.addKnob (knob (P::close, "Close"));
            s.addKnob (knob (P::overhead, "Overhead"));
            s.addKnob (knob (P::room, "Room"));
            s.addKnob (knob (P::volume, "Volume", S::large));
        }

        addAndMakeVisible (presetBar);
        addAndMakeVisible (page);

        pads.onPlay = [this] (int note, int velocity)
        {
            if (onPreviewNote)
                onPreviewNote (note, velocity);
        };
        addAndMakeVisible (pads);

        setSize (juce::jmax (minWidth, page.getPreferredWidth()),
                 PresetBar::height + page.getPreferredHeight() + padsHeight + margin);

        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff232327));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        presetBar.setBounds (area.removeFromTop (PresetBar::height));
        page.setBounds (area.removeFromTop (page.getPreferredHeight()));
        pads.setBounds (area.reduced (margin, 0).withTrimmedBottom (margin));
    }

private:
    static constexpr int minWidth = 560;
    static constexpr int padsHeight = 92;
    static constexpr int margin = 6;

    // One pad per note, in the order of DrumModelPlugin::getNotes(). The upper
    // half of a pad plays it hard, the lower half softly.
    class Pads : public juce::Component
    {
    public:
        std::function<void (int note, int velocity)> onPlay;
        std::array<float, 13> glow {};
        juce::String status;

        void paint (juce::Graphics& g) override
        {
            const auto& notes = plugins::DrumModelPlugin::getNotes();
            g.setFont (uiFont (fonts::small));

            for (int i = 0; i < (int) notes.size(); ++i)
            {
                const auto r = padBounds (i);
                const float lit = glow[(size_t) i];
                g.setColour (juce::Colour (0xff2e2e34).interpolatedWith (juce::Colour (0xffe8a33c), 0.15f + 0.55f * lit));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (juce::Colour (0xffd8d8de));
                g.drawFittedText (plugins::drummodel::pieceName (notes[(size_t) i].piece), r.reduced (3.0f).toNearestInt(),
                                  juce::Justification::centred, 2, 0.8f);
            }

            if (status.isNotEmpty())
            {
                g.setColour (juce::Colour (0xffa19787));
                g.drawText (status, getLocalBounds().removeFromBottom (16), juce::Justification::centredLeft);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            for (int i = 0; i < 13; ++i)
            {
                const auto r = padBounds (i);
                if (r.contains (e.position) && onPlay)
                {
                    const float depth = (e.position.y - r.getY()) / r.getHeight();
                    onPlay (plugins::DrumModelPlugin::getNotes()[(size_t) i].note, juce::jlimit (1, 127, juce::roundToInt (127.0f * (1.0f - 0.8f * depth))));
                }
            }
        }

    private:
        juce::Rectangle<float> padBounds (int i) const
        {
            const float gap = 4.0f, w = ((float) getWidth() - 12 * gap) / 13.0f;
            return { (float) i * (w + gap), 0.0f, w, (float) getHeight() - 20.0f };
        }
    };

    void timerCallback() override
    {
        auto* kit = dynamic_cast<plugins::DrumModelPlugin*> (plugin.get());

        if (kit == nullptr)
        {
            // EditSync deleted the plugin under us; the window is about to be
            // closed, so stop touching anything until it is.
            setEnabled (false);
            stopTimer();
            return;
        }

        page.refresh();

        using Piece = plugins::drummodel::Piece;
        const auto& a = kit->getActivity();
        const int toms[] { a.toms[0].load(), a.toms[1].load(), a.toms[2].load() };
        const bool cymbals = a.cymbals.load() > 0;
        bool changed = false;
        const auto& notes = plugins::DrumModelPlugin::getNotes();

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto piece = notes[i].piece;
            const bool active = piece == Piece::kick ? a.kick.load() > 0
                              : piece == Piece::snare || piece == Piece::rimshot ? a.snare.load() > 0
                              : piece == Piece::tomHigh ? toms[0] > 0
                              : piece == Piece::tomMid ? toms[1] > 0
                              : piece == Piece::tomFloor ? toms[2] > 0
                              : cymbals;
            const float target = active ? 1.0f : 0.0f;
            const float next = pads.glow[i] + (target - pads.glow[i]) * (active ? 1.0f : 0.25f);
            changed = changed || std::abs (next - pads.glow[i]) > 1e-3f;
            pads.glow[i] = std::abs (next - target) < 1e-3f ? target : next;
        }

        const juce::String status = kit->failedToLoad() ? juce::String ("The kit's models could not be loaded.")
                                  : kit->isLoaded() ? juce::String ("Kick, snare and toms computed as they are hit; cymbals sampled from the same model.")
                                                    : juce::String ("Settling the snare wires...");
        if (status != pads.status)
        {
            pads.status = status;
            changed = true;
        }

        if (changed)
            pads.repaint();
    }

    te::SafeSelectable<te::Plugin> plugin;
    PresetBar presetBar;
    KnobPage page;
    Pads pads;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumModelEditor)
};

} // namespace carve::app
