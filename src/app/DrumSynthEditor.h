#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Fonts.h"
#include "KnobPanel.h"
#include "PresetManager.h"
#include "plugins/DrumSynthPlugin.h"

namespace carve::app
{

// The editor for the built-in drum machine, laid out like the machine's own
// front panel: one column per drum, its pad at the top and its knobs under
// it, so the pad you just hit is over the knobs that change it. The level
// knobs line up across the top of every column and read as the kit's mixer;
// what else a drum has -- a decay, the kick's tune, sweep, drive and click --
// sits below in smaller knobs.
//
// Small on purpose -- the synth is (see DrumSynthPlugin.h). The three toms
// share one column, since they share their decay and level.
class DrumSynthEditor : public juce::Component,
                        private juce::Timer
{
public:
    using Drum = plugins::DrumSynthPlugin::Drum;

    // Plays a drum through the generator's track, the way the piano roll's
    // previews do. Wired by the generator window.
    std::function<void (int note, int velocity)> onPreviewNote;

    explicit DrumSynthEditor (plugins::DrumSynthPlugin& drums)
        : plugin (drums), presetBar (drums)
    {
        auto isAlive = [this] { return plugin != nullptr; };
        using S = KnobSize;

        auto knob = [&isAlive] (te::AutomatableParameter::Ptr parameter, const juce::String& name, KnobSize size)
        {
            return std::make_unique<ParameterKnob> (*parameter, name, size, isAlive);
        };

        // Kick: the one drum with a sound of its own to shape, two knobs wide.
        {
            auto& column = addColumn ({ Drum::kick }, 2);
            column.section->addKnob (knob (drums.kickLevel, "Level", S::large));
            column.section->newRow();
            column.section->addKnob (knob (drums.kickTune, "Tune", S::medium));
            column.section->addKnob (knob (drums.kickDecay, "Decay", S::medium));
            column.section->newRow();
            column.section->addKnob (knob (drums.kickSweep, "Sweep", S::small));
            column.section->addKnob (knob (drums.kickDrive, "Drive", S::small));
            column.section->addKnob (knob (drums.kickClick, "Click", S::small));
        }
        {
            auto& column = addColumn ({ Drum::rim }, 1);
            column.section->addKnob (knob (drums.rimLevel, "Level", S::large));
        }
        {
            auto& column = addColumn ({ Drum::snare }, 1);
            column.section->addKnob (knob (drums.snareLevel, "Level", S::large));
            column.section->newRow();
            column.section->addKnob (knob (drums.snareSnappy, "Snappy", S::small));
            column.section->newRow();
            column.section->addKnob (knob (drums.snareDecay, "Decay", S::small));
        }
        {
            auto& column = addColumn ({ Drum::clap }, 1);
            column.section->addKnob (knob (drums.clapLevel, "Level", S::large));
            column.section->newRow();
            column.section->addKnob (knob (drums.clapDecay, "Decay", S::medium));
        }
        {
            auto& column = addColumn ({ Drum::closedHat }, 1);
            column.section->addKnob (knob (drums.closedHatLevel, "Level", S::large));
            column.section->newRow();
            column.section->addKnob (knob (drums.closedHatDecay, "Decay", S::medium));
        }
        {
            auto& column = addColumn ({ Drum::openHat }, 1);
            column.section->addKnob (knob (drums.openHatLevel, "Level", S::large));
            column.section->newRow();
            column.section->addKnob (knob (drums.openHatDecay, "Decay", S::medium));
        }
        {
            auto& column = addColumn ({ Drum::lowTom, Drum::midTom, Drum::highTom }, 2);
            column.section->addKnob (knob (drums.tomLevel, "Level", S::large));
            column.section->newRow();
            column.section->addKnob (knob (drums.tomDecay, "Decay", S::medium));
        }
        {
            auto& column = addColumn ({ Drum::cowbell }, 1);
            column.section->addKnob (knob (drums.cowbellLevel, "Level", S::large));
        }

        addAndMakeVisible (presetBar);

        int width = margin * 2 - columnGap;
        int columnHeight = 0;

        for (auto& column : columns)
        {
            width += getColumnWidth (*column) + columnGap;
            columnHeight = juce::jmax (columnHeight, padHeight + column->section->getPreferredHeight());
        }

        setSize (width, PresetBar::height + margin * 2 + columnHeight);

        for (auto& column : columns)
            for (auto& pad : column->pads)
                pad->resetActivity (drums.getActivity (pad->getDrum()).hits);

        startTimerHz (refreshHz);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff232327));

        g.setColour (juce::Colour (0xff2a2a31));

        for (auto& column : columns)
            g.fillRoundedRectangle (column->bounds.toFloat(), 3.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        presetBar.setBounds (area.removeFromTop (PresetBar::height));
        area.reduce (margin, margin);

        for (auto& column : columns)
        {
            column->bounds = area.removeFromLeft (getColumnWidth (*column));
            area.removeFromLeft (columnGap);

            auto inner = column->bounds;
            auto padRow = inner.removeFromTop (padHeight).reduced (4, 4);
            const int padWidth = padRow.getWidth() / (int) column->pads.size();

            for (auto& pad : column->pads)
                pad->setBounds (padRow.removeFromLeft (padWidth).reduced (1, 0));

            column->section->setBounds (inner.withHeight (column->section->getPreferredHeight()));
        }
    }

private:
    // One drum's pad(s) and knobs. `units` is how many knob-widths across.
    // A pad: the drum's name over its note, at a size that fits a column
    // one knob wide. A TextButton's font follows its height, and at pad
    // height that wrapped "Snare" onto two lines.
    class Pad : public juce::Button
    {
    public:
        Pad (Drum drumToPlay, const juce::String& drumName, int midiNote)
            : juce::Button (drumName), drum (drumToPlay), name (drumName),
              note (juce::MidiMessage::getMidiNoteName (midiNote, true, true, 3))
        {
            setTooltip ("MIDI note " + juce::String (midiNote));
            setWantsKeyboardFocus (false);
        }

        Drum getDrum() const  { return drum; }

        // Lit while the drum sounds, from the synth's own voices -- the same
        // light a drum kit's pads give, and the same accent colour, so the two
        // instruments read alike.
        //
        // A hit shorter than the poll interval would never be caught by the
        // voice alone, so a moved hit count lights the pad for a minimum flash
        // whatever the voice says by the time it is looked at.
        void showActivity (const plugins::DrumSynthPlugin::DrumActivity& activity, double nowMs)
        {
            if (activity.hits != seenHits)
            {
                seenHits = activity.hits;
                lastHitMs = nowMs;
            }

            const bool flashing = lastHitMs > 0.0 && nowMs - lastHitMs < minimumFlashSeconds * 1000.0;
            setLit (flashing || activity.sounding);
        }

        // Starts from a hit count rather than from zero, or every drum played
        // since the song loaded would flash the moment the window opened.
        void resetActivity (std::uint32_t hits)
        {
            seenHits = hits;
            lastHitMs = 0.0;
            setLit (false);
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            auto area = getLocalBounds().toFloat();
            const bool bright = down || lit;
            g.setColour (bright ? juce::Colour (0xffe0a24f)
                                : highlighted ? juce::Colour (0xff4a4a54) : juce::Colour (0xff3a3a42));
            g.fillRoundedRectangle (area, 4.0f);

            const auto text = bright ? juce::Colour (0xff232327) : juce::Colour (0xffe8e8ec);
            auto bounds = getLocalBounds();
            g.setColour (text);
            g.setFont (uiFont (fonts::small, juce::Font::bold));
            g.drawText (name, bounds.removeFromTop (bounds.getHeight() / 2 + 2), juce::Justification::centredBottom);
            g.setColour (text.withAlpha (0.7f));
            g.setFont (uiFont (fonts::small));
            g.drawText (note, bounds, juce::Justification::centredTop);
        }

    private:
        void setLit (bool shouldBeLit)
        {
            if (lit == shouldBeLit)
                return;

            lit = shouldBeLit;
            repaint();
        }

        Drum drum;
        juce::String name, note;

        std::uint32_t seenHits = 0;
        double lastHitMs = 0.0;   // 0 = never, on the hi-res clock
        bool lit = false;
    };

    struct Column
    {
        std::vector<std::unique_ptr<Pad>> pads;
        std::unique_ptr<KnobSection> section;
        int units = 1;
        juce::Rectangle<int> bounds;
    };

    static constexpr int unitWidth = 62;
    static constexpr int columnGap = 6;
    static constexpr int margin = 6;
    static constexpr int padHeight = 48;
    static constexpr int previewVelocity = 100;

    // How often the pads and the knobs are looked at, and the least a pad
    // stays lit once hit -- the drum kit's grid uses the same pair, so a hat
    // roll flickers per hit on both.
    static constexpr int refreshHz = 30;
    static constexpr double minimumFlashSeconds = 0.1;

    static int getColumnWidth (const Column& column)
    {
        return column.units * unitWidth + (column.units - 1) * columnGap;
    }

    Column& addColumn (std::initializer_list<Drum> drums, int units)
    {
        auto column = std::make_unique<Column>();
        column->units = units;

        // Short names on the pads -- "Low Tom" does not fit a pad -- with the
        // note under each, so the pads double as the key map for drawing a
        // pattern.
        static const char* padNames[] { "Kick", "Rim", "Snare", "Clap", "CH", "OH",
                                        "LT", "MT", "HT", "Bell" };

        for (auto drum : drums)
        {
            const int note = plugins::DrumSynthPlugin::getNoteForDrum (drum);

            auto pad = std::make_unique<Pad> (drum, padNames[(int) drum], note);
            pad->onClick = [this, note]
            {
                if (onPreviewNote)
                    onPreviewNote (note, previewVelocity);
            };
            addAndMakeVisible (*pad);
            column->pads.push_back (std::move (pad));
        }

        // The pad is the column's title, and the column box is painted here,
        // around pad and knobs together.
        column->section = std::make_unique<KnobSection> (juce::String());
        column->section->setCentred (true);
        column->section->setDrawsBackground (false);
        addAndMakeVisible (*column->section);

        columns.push_back (std::move (column));
        return *columns.back();
    }

    void timerCallback() override
    {
        if (plugin == nullptr)
        {
            // EditSync deleted the plugin under us; the window is about to be
            // closed, so stop touching anything until it is.
            setEnabled (false);
            stopTimer();
            return;
        }

        for (auto& column : columns)
            column->section->refresh();

        if (auto* drums = dynamic_cast<plugins::DrumSynthPlugin*> (plugin.get()))
        {
            const auto now = juce::Time::getMillisecondCounterHiRes();

            for (auto& column : columns)
                for (auto& pad : column->pads)
                    pad->showActivity (drums->getActivity (pad->getDrum()), now);
        }
    }

    te::SafeSelectable<te::Plugin> plugin;
    PresetBar presetBar;
    std::vector<std::unique_ptr<Column>> columns;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumSynthEditor)
};

} // namespace carve::app
