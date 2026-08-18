#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "ParameterRows.h"
#include "PresetManager.h"

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
            g.setFont (juce::FontOptions (12.0f));
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

// The panel above in a floating window, matching PluginEditorWindow's
// behaviour so the mixer can own both through one pointer.
class EffectParameterWindow : public juce::DocumentWindow
{
public:
    EffectParameterWindow (te::Plugin& plugin, std::function<void()> onCloseCallback)
        : juce::DocumentWindow (plugin.getName(),
                                juce::Colour (0xff232327),
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          content (plugin)
    {
        const auto panelHeight = juce::jlimit (60, maxHeight, content.getPanelHeight());

        content.setSize (EffectParameterPanel::width, panelHeight + PresetBar::height);

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
        explicit Content (te::Plugin& plugin)
            : presetBar (plugin)
        {
            auto panel = std::make_unique<EffectParameterPanel> (plugin);
            panelHeight = panel->getHeight();

            viewport.setViewedComponent (panel.release(), true);
            viewport.setScrollBarsShown (true, false);

            addAndMakeVisible (presetBar);
            addAndMakeVisible (viewport);
        }

        int getPanelHeight() const  { return panelHeight; }

        void resized() override
        {
            auto area = getLocalBounds();
            presetBar.setBounds (area.removeFromTop (PresetBar::height));
            viewport.setBounds (area);

            // The panel is as tall as its parameter list; only the viewport
            // follows the window.
            if (auto* panel = viewport.getViewedComponent())
                panel->setSize (viewport.getMaximumVisibleWidth(), panel->getHeight());
        }

    private:
        PresetBar presetBar;
        juce::Viewport viewport;
        int panelHeight = 0;
    };

    std::function<void()> onClose;
    Content content;
};

} // namespace carve::app
