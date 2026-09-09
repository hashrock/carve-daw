#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "ParameterRows.h"
#include "PresetManager.h"
#include "../plugins/MeteredCompressorPlugin.h"

namespace te = tracktion;

namespace carve::app
{

// A generic editor for a tracktion internal plugin: one slider per
// AutomatableParameter.
//
// Internal plugins are not juce::AudioProcessors, so PluginEditorWindow's
// GenericAudioProcessorEditor is not an option for them -- but they do publish
// named parameters with real ranges and value strings, which is enough to build
// the same thing by hand. Without it a compressor could only ever be inserted
// at its defaults.
//
// Parameter moves go straight at the plugin rather than through the song model,
// which is where the rest of the mixer's edits go: the plugin's own ValueTree
// is the parameter store, and EditSync copies it back into the model when the
// song is saved. The cost is that a knob move is not undoable.
//
// The 4OSC editor is the same rows in a structured layout; this one stays a
// flat list, which is all the parameter list of an effect really is.
class EffectParameterPanel : public juce::Component,
                             private juce::Timer
{
public:
    explicit EffectParameterPanel (te::Plugin& pluginToEdit)
        : plugin (pluginToEdit)
    {
        for (auto* parameter : pluginToEdit.getAutomatableParameters())
            if (parameter != nullptr)
                rows.push_back (std::make_unique<ParameterSliderRow> (
                                    *parameter, [this] { return plugin != nullptr; }));

        for (auto& row : rows)
            addAndMakeVisible (*row);

        setSize (width, juce::jmax (rowHeight, (int) rows.size() * rowHeight));
        startTimerHz (15);
    }

    static constexpr int width = 320;
    static constexpr int rowHeight = EditorRow::height;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff232327));

        if (rows.empty())
        {
            g.setColour (juce::Colour (0xff9a9aa4));
            g.setFont (juce::FontOptions (13.0f));
            g.drawText ("This effect has no editable parameters.",
                        getLocalBounds(), juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();

        for (auto& row : rows)
            row->setBounds (area.removeFromTop (rowHeight));
    }

private:
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

        for (auto& row : rows)
            row->refresh();
    }

    te::SafeSelectable<te::Plugin> plugin;
    std::vector<std::unique_ptr<ParameterSliderRow>> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectParameterPanel)
};

// The compressor's gain-reduction meter: a bar growing leftwards from 0dB,
// laid out like a parameter row so it reads as part of the same list.
//
// Polled on its own timer rather than through the panel's refresh: a meter
// wants to move at a rate a slider list has no reason to repaint at. The
// plugin publishes one atomic float per block, so a tick costs a load and a
// repaint. Fall-off is applied here, not in the plugin: the reading is per
// block and would flicker at the block rate otherwise.
class GainReductionMeter : public juce::Component,
                           private juce::Timer
{
public:
    explicit GainReductionMeter (plugins::MeteredCompressorPlugin& compressorToWatch)
        : compressor (compressorToWatch)
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (30);
    }

    static constexpr int height = EditorRow::height;

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().reduced (6, 2);

        g.setColour (juce::Colour (0xffd8d8dc));
        g.setFont (juce::FontOptions (EditorRow::rowFontHeight));
        g.drawText ("Reduction", area.removeFromLeft (nameWidth), juce::Justification::centredLeft);

        auto valueArea = area.removeFromRight (valueWidth);
        g.setColour (juce::Colour (0xff9a9aa4));
        g.drawText (juce::String (-shownDb, 1) + " dB", valueArea, juce::Justification::centredRight);

        const auto track = area.reduced (4, 0).toFloat();
        auto bar = track.reduced (0.0f, 6.0f);
        g.setColour (juce::Colour (0xff1c1c20));
        g.fillRect (bar);

        // 0dB sits at the right, so the bar reaches left as the compressor
        // bites -- the direction every hardware reduction meter swings.
        const auto proportion = juce::jlimit (0.0f, 1.0f, shownDb / rangeDb);
        g.setColour (juce::Colour (0xffe0a24f));
        g.fillRect (bar.removeFromRight (bar.getWidth() * proportion));

        // Ticks every 6dB above the bar, so a reading has a scale.
        g.setColour (juce::Colour (0xff707078));

        for (float db = 6.0f; db < rangeDb; db += 6.0f)
            g.drawVerticalLine ((int) (track.getRight() - track.getWidth() * (db / rangeDb)),
                                track.getY() + 1.0f, track.getY() + 5.0f);
    }

private:
    static constexpr int nameWidth = 100;    // as ParameterSliderRow lays out
    static constexpr int valueWidth = 64;
    static constexpr float rangeDb = 24.0f;

    // dB per tick: at 30Hz a 30dB/s fall, slow enough to read a transient.
    static constexpr float decayDb = 1.0f;

    void timerCallback() override
    {
        // EditSync can delete the plugin under an open window; a bypassed one
        // is not called at all, so its last reading would otherwise stick.
        const auto live = compressor != nullptr && compressor->isEnabled()
                            ? -compressor->getGainReductionDb()
                            : 0.0f;

        shownDb = juce::jmax (live, shownDb - decayDb);
        repaint();
    }

    te::SafeSelectable<plugins::MeteredCompressorPlugin> compressor;
    float shownDb = 0.0f;   // magnitude, so the maths above reads as a level

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GainReductionMeter)
};

// What the sidechain picker needs to know and do. Supplied by the owner for
// effects that have a sidechain input (the compressor); the window itself
// knows nothing about generators or the model.
struct SidechainPicker
{
    // {generatorId, display name}, in mixer order.
    std::vector<std::pair<juce::String, juce::String>> sources;
    juce::String currentSourceId;
    std::function<void (const juce::String&)> onSourceChanged;   // empty = none
};

// The panel above in a floating window, matching PluginEditorWindow's
// behaviour so the mixer can own both through one pointer.
class EffectParameterWindow : public juce::DocumentWindow
{
public:
    EffectParameterWindow (te::Plugin& plugin, std::function<void()> onCloseCallback,
                           std::optional<SidechainPicker> sidechainPicker = std::nullopt)
        : juce::DocumentWindow (plugin.getName(),
                                juce::Colour (0xff232327),
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          content (plugin, std::move (sidechainPicker))
    {
        const auto panelHeight = juce::jlimit (60, maxHeight, content.getPanelHeight());

        content.setSize (EffectParameterPanel::width,
                         panelHeight + PresetBar::height + content.getExtraRowsHeight());

        setContentNonOwned (&content, true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (EffectParameterPanel::width, 60 + PresetBar::height,
                         EffectParameterPanel::width, maxHeight + PresetBar::height);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();   // owner destroys this window (deferred)
    }

private:
    static constexpr int maxHeight = 460;

    // The preset strip has to stay put while the parameters scroll, so the
    // viewport is a child here rather than the window's content itself.
    class Content : public juce::Component
    {
    public:
        Content (te::Plugin& plugin, std::optional<SidechainPicker> picker)
            : presetBar (plugin)
        {
            if (picker)
            {
                sidechain = std::move (picker);

                sidechainBox = std::make_unique<juce::ComboBox>();
                sidechainBox->addItem ("Sidechain: none", 1);

                int itemId = 2, selected = 1;

                for (const auto& [id, name] : sidechain->sources)
                {
                    sidechainBox->addItem ("Sidechain: " + name, itemId);
                    if (id == sidechain->currentSourceId)
                        selected = itemId;
                    ++itemId;
                }

                sidechainBox->setSelectedId (selected, juce::dontSendNotification);
                sidechainBox->onChange = [this]
                {
                    const auto index = sidechainBox->getSelectedId() - 2;
                    const auto& sources = sidechain->sources;

                    if (sidechain->onSourceChanged)
                        sidechain->onSourceChanged (index >= 0 && index < (int) sources.size()
                                                        ? sources[(size_t) index].first
                                                        : juce::String());
                };

                addAndMakeVisible (*sidechainBox);
            }

            if (auto* compressor = dynamic_cast<plugins::MeteredCompressorPlugin*> (&plugin))
            {
                meter = std::make_unique<GainReductionMeter> (*compressor);
                addAndMakeVisible (*meter);
            }

            auto panel = std::make_unique<EffectParameterPanel> (plugin);
            panelHeight = panel->getHeight();

            viewport.setViewedComponent (panel.release(), true);
            viewport.setScrollBarsShown (true, false);

            addAndMakeVisible (presetBar);
            addAndMakeVisible (viewport);
        }

        int getPanelHeight() const  { return panelHeight; }
        int getExtraRowsHeight() const
        {
            return (sidechainBox != nullptr ? sidechainRowHeight : 0)
                 + (meter != nullptr ? GainReductionMeter::height : 0);
        }

        static constexpr int sidechainRowHeight = 28;

        void resized() override
        {
            auto area = getLocalBounds();
            presetBar.setBounds (area.removeFromTop (PresetBar::height));

            if (sidechainBox != nullptr)
                sidechainBox->setBounds (area.removeFromTop (sidechainRowHeight).reduced (6, 3));

            if (meter != nullptr)
                meter->setBounds (area.removeFromTop (GainReductionMeter::height));

            viewport.setBounds (area);

            // The panel is as tall as its parameter list; only the viewport
            // follows the window.
            if (auto* panel = viewport.getViewedComponent())
                panel->setSize (viewport.getMaximumVisibleWidth(), panel->getHeight());
        }

    private:
        PresetBar presetBar;
        std::optional<SidechainPicker> sidechain;
        std::unique_ptr<juce::ComboBox> sidechainBox;
        std::unique_ptr<GainReductionMeter> meter;
        juce::Viewport viewport;
        int panelHeight = 0;
    };

    std::function<void()> onClose;
    Content content;
};

} // namespace carve::app
