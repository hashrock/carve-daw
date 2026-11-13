#pragma once

#include <array>
#include <functional>
#include <memory>

#include "Fonts.h"
#include "KnobPanel.h"
#include "PresetManager.h"
#include "plugins/PianoModel.h"
#include "plugins/PianoPlugin.h"

namespace carve::app
{

// The editor for the modelled piano: its knobs, grouped by where in the
// instrument they act -- the hammer, the strings, the case and the room --
// over the keyboard it plays, which lights up the keys held down and plays a
// key when one is clicked. The pedal switch above it holds the dampers up:
// MIDI keyboards here send notes only, so this is the piano's pedal.
class PianoEditor : public juce::Component,
                    private juce::Timer
{
public:
    // Plays a note through the generator's track, the way the piano roll's
    // previews do. Wired by the generator window.
    std::function<void (int note, int velocity)> onPreviewNote;

    explicit PianoEditor (plugins::PianoPlugin& pianoToEdit)
        : plugin (pianoToEdit), presetBar (pianoToEdit)
    {
        auto isAlive = [this] { return plugin != nullptr; };
        using S = KnobSize;

        auto knob = [&isAlive] (te::AutomatableParameter::Ptr parameter, const juce::String& name, KnobSize size)
        {
            return std::make_unique<ParameterKnob> (*parameter, name, size, isAlive);
        };

        {
            auto& s = page.add (std::make_unique<KnobSection> ("Hammer"));
            s.addKnob (knob (pianoToEdit.hardness, "Hardness", S::large));
            s.addKnob (knob (pianoToEdit.knock, "Knock", S::medium));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Strings"));
            s.addKnob (knob (pianoToEdit.detune, "Detune", S::medium));
            s.addKnob (knob (pianoToEdit.sympathetic, "Resonance", S::medium));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Body"));
            s.addKnob (knob (pianoToEdit.brightness, "Bright", S::medium));
            s.addKnob (knob (pianoToEdit.board, "Board", S::medium));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Room"));
            s.addKnob (knob (pianoToEdit.reverb, "Room", S::medium));
            s.addKnob (knob (pianoToEdit.volume, "Volume", S::large));
        }

        addAndMakeVisible (presetBar);
        addAndMakeVisible (page);

        keyboard.onKey = [this] (int note, int velocity)
        {
            if (onPreviewNote)
                onPreviewNote (note, velocity);
        };
        keyboard.onPedalClicked = [this]
        {
            if (auto* piano = dynamic_cast<plugins::PianoPlugin*> (plugin.get()))
            {
                auto& parameter = *piano->pedal;
                parameter.parameterChangeGestureBegin();
                parameter.setParameter (parameter.getCurrentValue() >= 0.5f ? 0.0f : 1.0f, juce::sendNotification);
                parameter.parameterChangeGestureEnd();
            }
        };
        addAndMakeVisible (keyboard);

        setSize (juce::jmax (minWidth, page.getPreferredWidth()),
                 PresetBar::height + page.getPreferredHeight() + keyboardHeight + margin);

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
        keyboard.setBounds (area.reduced (margin, 0).withTrimmedBottom (margin));
    }

private:
    static constexpr int minWidth = 560;
    static constexpr int keyboardHeight = 92;
    static constexpr int margin = 6;

    // The 88 keys, A0 to C8. Lower down a key is a harder strike, as on
    // the prototype's.
    class Keyboard : public juce::Component
    {
    public:
        std::function<void (int note, int velocity)> onKey;
        std::function<void()> onPedalClicked;

        // `pedalSwitch` is the switch; `pedal` whether the pedal is down at
        // all, which a keyboard's CC64 can also make it.
        void setState (std::uint64_t low, std::uint64_t high, bool pedalSwitch, bool pedal, int voices)
        {
            if (low == down[0] && high == down[1] && pedalSwitch == switchOn && pedal == pedalDown && voices == voiceCount)
                return;

            down = { low, high };
            switchOn = pedalSwitch;
            pedalDown = pedal;
            voiceCount = voices;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto area = getLocalBounds();
            auto status = area.removeFromTop (statusHeight);

            g.setFont (uiFont (fonts::small));

            // The pedal switch: a pill, lit while the pedal is down.
            const auto pill = pedalBounds().toFloat().reduced (0.5f, 1.5f);
            g.setColour (switchOn ? juce::Colour (0xffe0a24f) : juce::Colour (0xff3a3a42));
            g.fillRoundedRectangle (pill, pill.getHeight() / 2.0f);
            g.setColour (switchOn ? juce::Colour (0xff232327) : pedalDown ? juce::Colour (0xffe0a24f) : juce::Colour (0xffc8c8d0));
            g.drawText (pedalDown ? "Pedal down" : "Pedal up", pill, juce::Justification::centred);

            g.setColour (juce::Colour (0xff8a8a94));
            g.drawText (juce::String (voiceCount) + (voiceCount == 1 ? " key sounding" : " keys sounding"),
                        status, juce::Justification::centredRight);

            keys = area;
            const float w = keyWidth();

            for (int m = plugins::piano::lowestKey; m <= plugins::piano::highestKey; ++m)
            {
                if (isBlack (m))
                    continue;

                auto r = whiteBounds (m, w);
                g.setColour (isDown (m) ? juce::Colour (0xffe0a24f) : juce::Colour (0xffe8e8ec));
                g.fillRect (r.reduced (0.5f, 0.0f));

                if (m % 12 == 0 && w > 6.0f)
                {
                    g.setColour (juce::Colour (0xff6a6a74));
                    g.setFont (uiFont (fonts::small * 0.85f));
                    g.drawText ("C" + juce::String (m / 12 - 1), r.withTrimmedTop (r.getHeight() - 14.0f),
                                juce::Justification::centred);
                }
            }

            for (int m = plugins::piano::lowestKey; m <= plugins::piano::highestKey; ++m)
            {
                if (! isBlack (m))
                    continue;

                g.setColour (isDown (m) ? juce::Colour (0xffc0802f) : juce::Colour (0xff1a1a1e));
                g.fillRect (blackBounds (m, w));
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (pedalBounds().contains (e.getPosition()))
            {
                if (onPedalClicked)
                    onPedalClicked();

                return;
            }

            if (const int note = noteAt (e.position); note >= 0 && onKey)
            {
                const float depth = juce::jlimit (0.0f, 1.0f, (e.position.y - (float) keys.getY()) / (float) keys.getHeight());
                onKey (note, juce::jlimit (1, 127, juce::roundToInt ((0.2f + 0.75f * depth) * 127.0f)));
            }
        }

    private:
        static constexpr int statusHeight = 20;

        juce::Rectangle<int> pedalBounds() const  { return getLocalBounds().removeFromTop (statusHeight).removeFromLeft (84); }
        static constexpr int numWhites = 52;

        static bool isBlack (int m)
        {
            const int k = m % 12;
            return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
        }

        static int whiteIndex (int m)
        {
            int index = 0;

            for (int k = plugins::piano::lowestKey; k < m; ++k)
                if (! isBlack (k))
                    ++index;

            return index;
        }

        float keyWidth() const  { return (float) keys.getWidth() / (float) numWhites; }

        juce::Rectangle<float> whiteBounds (int m, float w) const
        {
            return { (float) keys.getX() + (float) whiteIndex (m) * w, (float) keys.getY(), w, (float) keys.getHeight() };
        }

        juce::Rectangle<float> blackBounds (int m, float w) const
        {
            const float x = (float) keys.getX() + (float) whiteIndex (m) * w - w * 0.3f;
            return { x, (float) keys.getY(), w * 0.6f, (float) keys.getHeight() * 0.62f };
        }

        bool isDown (int m) const  { return (down[(size_t) (m >> 6)] >> (m & 63)) & 1; }

        int noteAt (juce::Point<float> p) const
        {
            const float w = keyWidth();

            for (int m = plugins::piano::lowestKey; m <= plugins::piano::highestKey; ++m)
                if (isBlack (m) && blackBounds (m, w).contains (p))
                    return m;

            for (int m = plugins::piano::lowestKey; m <= plugins::piano::highestKey; ++m)
                if (! isBlack (m) && whiteBounds (m, w).contains (p))
                    return m;

            return -1;
        }

        juce::Rectangle<int> keys;
        std::array<std::uint64_t, 2> down {};
        bool switchOn = false, pedalDown = false;
        int voiceCount = 0;
    };

    void timerCallback() override
    {
        auto* piano = dynamic_cast<plugins::PianoPlugin*> (plugin.get());

        if (piano == nullptr)
        {
            // EditSync deleted the plugin under us; the window is about to be
            // closed, so stop touching anything until it is.
            setEnabled (false);
            stopTimer();
            return;
        }

        page.refresh();
        keyboard.setState (piano->getKeysDown (0), piano->getKeysDown (1), piano->pedal->getCurrentValue() >= 0.5f,
                           piano->isPedalDown(), piano->getVoiceCount());
    }

    te::SafeSelectable<te::Plugin> plugin;
    PresetBar presetBar;
    KnobPage page;
    Keyboard keyboard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoEditor)
};

} // namespace carve::app
