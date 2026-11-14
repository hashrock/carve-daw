#pragma once

#include <array>
#include <cmath>
#include <functional>

#include "Fonts.h"
#include "KnobPanel.h"
#include "PresetManager.h"
#include "plugins/BassModel.h"
#include "plugins/BassModelPlugin.h"

namespace carve::app
{

// The editor for the modelled bass: its knobs, grouped by where in the
// instrument they act -- the right hand, the fretting hand, the strings, the
// pickups and the amp -- over a picture of the neck: the four strings as they
// actually move (magnified, from the stopped fret to the bridge), where each
// is stopped, and the two pickups. Clicking a string at a fret plays it.
class BassModelEditor : public juce::Component,
                        private juce::Timer
{
public:
    // Plays a note through the generator's track, the way the piano roll's
    // previews do. Wired by the generator window.
    std::function<void (int note, int velocity)> onPreviewNote;

    explicit BassModelEditor (plugins::BassModelPlugin& bassToEdit)
        : plugin (bassToEdit), presetBar (bassToEdit)
    {
        auto isAlive = [this] { return plugin != nullptr; };
        auto& state = bassToEdit.state;
        using S = KnobSize;

        auto knob = [&isAlive] (te::AutomatableParameter::Ptr parameter, const juce::String& name, KnobSize size)
        {
            return std::make_unique<ParameterKnob> (*parameter, name, size, isAlive);
        };

        {
            auto& s = page.add (std::make_unique<KnobSection> ("Right Hand"));
            s.addHeaderControl (std::make_unique<PropertyChoiceBox> (
                state, "style", juce::StringArray { "Fingers", "Pick", "Thumb", "Pop", "Slap & Pop" }, isAlive, 0));
            s.addKnob (knob (bassToEdit.pluck, "Position", S::large));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Fretting Hand"));
            s.addHeaderControl (std::make_unique<PropertyChoiceBox> (
                state, "legato", juce::StringArray { "Retrigger", "Hammer-on", "Slide" }, isAlive, 1));
            s.addHeaderControl (std::make_unique<PropertyToggleButton> (state, "fretless", "Fretless", isAlive, false));
            s.addKnob (knob (bassToEdit.slide, "Slide", S::medium));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Strings"));
            s.addKnob (knob (bassToEdit.action, "Action", S::medium));
            s.addKnob (knob (bassToEdit.sustain, "Sustain", S::medium));
            s.addKnob (knob (bassToEdit.bright, "Age", S::medium));
            s.addKnob (knob (bassToEdit.mute, "Mute", S::medium));
        }
        page.newRow();
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Pickups"));
            s.addKnob (knob (bassToEdit.pickup, "Neck/Bridge", S::large));
            s.addKnob (knob (bassToEdit.height, "Height", S::medium));
            s.addKnob (knob (bassToEdit.tone, "Tone", S::medium));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Amp"));
            s.addKnob (knob (bassToEdit.amp, "DI/Amp", S::medium));
            s.addKnob (knob (bassToEdit.drive, "Drive", S::medium));
            s.addKnob (knob (bassToEdit.volume, "Volume", S::large));
        }

        addAndMakeVisible (presetBar);
        addAndMakeVisible (page);

        neck.onPlay = [this] (int note, int velocity)
        {
            if (onPreviewNote)
                onPreviewNote (note, velocity);
        };
        addAndMakeVisible (neck);

        setSize (juce::jmax (minWidth, page.getPreferredWidth()),
                 PresetBar::height + page.getPreferredHeight() + neckHeight + margin);

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
        neck.setBounds (area.reduced (margin, 0).withTrimmedBottom (margin));
    }

private:
    static constexpr int minWidth = 560;
    static constexpr int neckHeight = 132;
    static constexpr int margin = 6;

    class Neck : public juce::Component
    {
    public:
        std::function<void (int note, int velocity)> onPlay;

        struct String { int fret = 0, held = -1; std::array<float, 32> shape {}; };
        std::array<String, plugins::bassmodel::numStrings> strings;

        void paint (juce::Graphics& g) override
        {
            using namespace plugins::bassmodel;
            auto area = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xff1c1c20));
            g.fillRoundedRectangle (area, 6.0f);

            g.setFont (uiFont (fonts::small));

            // Frets and pickups.
            for (int n = 0; n <= numFrets; ++n)
            {
                const float x = xAt (std::pow (2.0, -n / 12.0));
                g.setColour (juce::Colour (n == 0 ? 0xffb0b0b8 : 0xff5a5a64));
                g.fillRect (x - (n == 0 ? 2.0f : 0.75f), top() - 6.0f, n == 0 ? 4.0f : 1.5f, bottom() - top() + 12.0f);

                if ((n == 3 || n == 5 || n == 7 || n == 9 || n == 12 || n == 15 || n == 17 || n == 19) && n > 0)
                {
                    const float x0 = xAt (std::pow (2.0, -(n - 1) / 12.0));
                    g.setColour (juce::Colour (0xff3a3a42));
                    g.fillEllipse ((x0 + x) / 2.0f - 3.0f, (top() + bottom()) / 2.0f - 3.0f, 6.0f, 6.0f);
                }
            }

            g.setColour (juce::Colour (0x2ee0a24f));

            for (double p : pickupPositions)
                g.fillRect (xAt (p) - 8.0f, top() - 8.0f, 16.0f, bottom() - top() + 16.0f);

            // The strings: straight up to the stopped fret, then their real
            // displacement to the bridge, magnified.
            for (int si = 0; si < numStrings; ++si)
            {
                const auto& s = strings[(size_t) si];
                const float y0 = yOf (si);
                const double stopped = std::pow (2.0, -s.fret / 12.0);

                juce::Path path;
                path.startNewSubPath (xAt (1.0), y0);
                path.lineTo (xAt (stopped), y0);

                for (int k = 31; k >= 0; --k)
                {
                    const double d = (k + 0.5) / 32.0 * stopped;
                    path.lineTo (xAt (d), y0 - juce::jlimit (-10.0f, 10.0f, s.shape[(size_t) k] * 20.0f));
                }

                path.lineTo (xAt (0.0), y0);
                g.setColour (juce::Colour (0xffc9cdd4));
                g.strokePath (path, juce::PathStrokeType (1.0f + (float) (3 - si) * 0.5f));

                if (s.held >= 0 && s.fret > 0)
                {
                    const float x = (xAt (stopped) + xAt (stopped * std::pow (2.0, 1.0 / 12.0))) / 2.0f;
                    g.setColour (juce::Colour (0xffe0a24f));
                    g.fillEllipse (x - 6.0f, y0 - 6.0f, 12.0f, 12.0f);
                }

                g.setColour (juce::Colour (0xff8a8a94));
                g.drawText (juce::String::charToString ("EADG"[si]), juce::Rectangle<float> (2.0f, y0 - 8.0f, 14.0f, 16.0f),
                            juce::Justification::centred);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            using namespace plugins::bassmodel;

            // The nearest string, and the fret space the click is in (left of the nut: open).
            int si = 0;

            for (int k = 1; k < numStrings; ++k)
                if (std::abs (e.position.y - yOf (k)) < std::abs (e.position.y - yOf (si)))
                    si = k;

            int fret = 0;

            for (int n = 1; n <= numFrets; ++n)
                if (e.position.x > xAt (std::pow (2.0, -(n - 1) / 12.0)) && e.position.x <= xAt (std::pow (2.0, -n / 12.0)))
                    fret = n;

            if (onPlay)
                onPlay (tuning[(size_t) si] + fret, 100);
        }

    private:
        // d: distance from the bridge as a fraction of the scale. The nut on the left.
        float xAt (double d) const
        {
            const float l = 20.0f, r = (float) getWidth() - 12.0f;
            return r - (r - l) * (float) d;
        }

        float top() const     { return 18.0f; }
        float bottom() const  { return (float) getHeight() - 18.0f; }
        float yOf (int si) const  { return bottom() - (bottom() - top()) * (float) si / 3.0f; }
    };

    void timerCallback() override
    {
        auto* bass = dynamic_cast<plugins::BassModelPlugin*> (plugin.get());

        if (bass == nullptr)
        {
            // EditSync deleted the plugin under us; the window is about to be
            // closed, so stop touching anything until it is.
            setEnabled (false);
            stopTimer();
            return;
        }

        page.refresh();

        bool moving = false;

        for (int si = 0; si < plugins::bassmodel::numStrings; ++si)
        {
            const auto& from = bass->getStringState (si);
            auto& to = neck.strings[(size_t) si];
            const int fret = from.fret.load (std::memory_order_relaxed), held = from.held.load (std::memory_order_relaxed);
            moving = moving || fret != to.fret || held != to.held;
            to.fret = fret;
            to.held = held;

            for (size_t k = 0; k < to.shape.size(); ++k)
            {
                const float v = from.shape[k].load (std::memory_order_relaxed);
                moving = moving || std::abs (v - to.shape[k]) > 1e-7f;
                to.shape[k] = v;
            }
        }

        if (moving)
            neck.repaint();
    }

    te::SafeSelectable<te::Plugin> plugin;
    PresetBar presetBar;
    KnobPage page;
    Neck neck;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassModelEditor)
};

} // namespace carve::app
