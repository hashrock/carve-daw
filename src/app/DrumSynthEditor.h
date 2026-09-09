#pragma once

#include <functional>
#include <memory>

#include "FourOscEditor.h"     // EditorSectionPage
#include "ParameterRows.h"
#include "PresetManager.h"
#include "plugins/DrumSynthPlugin.h"

namespace carve::app
{

// The editor for the built-in drum machine: a row of pads to hear each drum,
// and the few knobs it has, grouped by drum. Small on purpose -- the synth
// is (see DrumSynthPlugin.h), and a page of empty sections would only say
// otherwise.
class DrumSynthEditor : public juce::Component,
                        private juce::Timer
{
public:
    using Drum = plugins::DrumSynthPlugin::Drum;

    // Plays a drum through the generator's track, the way the piano roll's
    // previews do. Wired by the generator window.
    std::function<void (int note, int velocity)> onPreviewNote;

    explicit DrumSynthEditor (plugins::DrumSynthPlugin& drums)
        : plugin (drums), presetBar (drums), page (2)
    {
        auto isAlive = [this] { return plugin != nullptr; };

        // One pad per drum, in the plugin's own order and with its note, so the
        // row doubles as the key map for drawing a pattern.
        for (int i = 0; i < plugins::DrumSynthPlugin::numDrums; ++i)
        {
            const auto drum = (Drum) i;
            const int note = plugins::DrumSynthPlugin::getNoteForDrum (drum);

            // Short names on the pads -- "Low Tom" wraps into nonsense at pad
            // width -- with the note under each, so the row is the key map.
            static const char* padNames[] { "Kick", "Rim", "Snare", "Clap", "CH", "OH",
                                            "LTom", "MTom", "HTom", "Bell" };

            auto pad = std::make_unique<juce::TextButton> (
                juce::String (padNames[i]) + "\n"
                    + juce::MidiMessage::getMidiNoteName (note, true, true, 3));
            pad->setTooltip ("MIDI note " + juce::String (note));
            pad->setWantsKeyboardFocus (false);
            pad->onClick = [this, note]
            {
                if (onPreviewNote)
                    onPreviewNote (note, previewVelocity);
            };
            addAndMakeVisible (*pad);
            pads.push_back (std::move (pad));
        }

        // Two columns, three rows. Every section ends on its level(s), so the
        // kit's balance reads down one edge of the page.
        auto kick = std::make_unique<EditorSection> ("Kick", nameWidth);
        kick->add (std::make_unique<ParameterSliderRow> (*drums.kickTune, "Tune", isAlive));
        kick->add (std::make_unique<ParameterSliderRow> (*drums.kickDecay, "Decay", isAlive));
        kick->add (std::make_unique<ParameterSliderRow> (*drums.kickSweep, "Sweep", isAlive));
        kick->add (std::make_unique<ParameterSliderRow> (*drums.kickDrive, "Drive", isAlive));
        kick->add (std::make_unique<ParameterSliderRow> (*drums.kickLevel, "Level", isAlive));
        page.add (std::move (kick));

        auto snare = std::make_unique<EditorSection> ("Snare", nameWidth);
        snare->add (std::make_unique<ParameterSliderRow> (*drums.snareSnappy, "Snappy", isAlive));
        snare->add (std::make_unique<ParameterSliderRow> (*drums.snareDecay, "Decay", isAlive));
        snare->add (std::make_unique<ParameterSliderRow> (*drums.snareLevel, "Level", isAlive));
        page.add (std::move (snare));

        auto hats = std::make_unique<EditorSection> ("Hi-hat", nameWidth);
        hats->add (std::make_unique<ParameterSliderRow> (*drums.closedHatDecay, "Closed Decay", isAlive));
        hats->add (std::make_unique<ParameterSliderRow> (*drums.openHatDecay, "Open Decay", isAlive));
        hats->add (std::make_unique<ParameterSliderRow> (*drums.closedHatLevel, "Closed Level", isAlive));
        hats->add (std::make_unique<ParameterSliderRow> (*drums.openHatLevel, "Open Level", isAlive));
        page.add (std::move (hats));

        auto clap = std::make_unique<EditorSection> ("Clap", nameWidth);
        clap->add (std::make_unique<ParameterSliderRow> (*drums.clapDecay, "Decay", isAlive));
        clap->add (std::make_unique<ParameterSliderRow> (*drums.clapLevel, "Level", isAlive));
        page.add (std::move (clap));

        auto toms = std::make_unique<EditorSection> ("Toms", nameWidth);
        toms->add (std::make_unique<ParameterSliderRow> (*drums.tomDecay, "Decay", isAlive));
        toms->add (std::make_unique<ParameterSliderRow> (*drums.tomLevel, "Level", isAlive));
        page.add (std::move (toms));

        auto percussion = std::make_unique<EditorSection> ("Rim / Cowbell", nameWidth);
        percussion->add (std::make_unique<ParameterSliderRow> (*drums.rimLevel, "Rim Level", isAlive));
        percussion->add (std::make_unique<ParameterSliderRow> (*drums.cowbellLevel, "Cowbell Level", isAlive));
        page.add (std::move (percussion));

        addAndMakeVisible (presetBar);
        addAndMakeVisible (page);

        setSize (width, PresetBar::height + padRowHeight + 8 + page.getPreferredHeight());
        startTimerHz (15);
    }

    static constexpr int width = 680;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff232327));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        presetBar.setBounds (area.removeFromTop (PresetBar::height));

        auto padRow = area.removeFromTop (padRowHeight).reduced (6, 4);
        const int padWidth = padRow.getWidth() / (int) pads.size();

        for (auto& pad : pads)
            pad->setBounds (padRow.removeFromLeft (padWidth).reduced (2, 0));

        area.removeFromTop (8);
        page.setBounds (area.removeFromTop (page.getPreferredHeight()));
    }

private:
    static constexpr int nameWidth = 96;
    static constexpr int padRowHeight = 48;
    static constexpr int previewVelocity = 100;

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

        page.refresh();
    }

    te::SafeSelectable<te::Plugin> plugin;
    PresetBar presetBar;
    std::vector<std::unique_ptr<juce::TextButton>> pads;
    EditorSectionPage page;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumSynthEditor)
};

} // namespace carve::app
