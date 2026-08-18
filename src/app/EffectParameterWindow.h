#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

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
class EffectParameterPanel : public juce::Component,
                             private juce::Timer
{
public:
    explicit EffectParameterPanel (te::Plugin& pluginToEdit)
        : plugin (pluginToEdit)
    {
        for (auto* parameter : pluginToEdit.getAutomatableParameters())
            if (parameter != nullptr)
                rows.push_back (std::make_unique<Row> (*parameter, [this] { return plugin != nullptr; }));

        for (auto& row : rows)
            addAndMakeVisible (*row);

        setSize (width, juce::jmax (rowHeight, (int) rows.size() * rowHeight));
        startTimerHz (15);
    }

    static constexpr int width = 320;
    static constexpr int rowHeight = 22;

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
    class Row : public juce::Component
    {
    public:
        Row (te::AutomatableParameter& parameterToEdit, std::function<bool()> isAliveCheck)
            : parameter (&parameterToEdit), isAlive (std::move (isAliveCheck))
        {
            nameLabel.setText (parameter->getParameterName(), juce::dontSendNotification);
            nameLabel.setFont (juce::FontOptions (11.0f));
            nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8dc));

            valueLabel.setFont (juce::FontOptions (11.0f));
            valueLabel.setJustificationType (juce::Justification::centredRight);
            valueLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));

            // Normalised: the parameter owns the taper, and its own value
            // string is what gets displayed anyway.
            slider.setSliderStyle (juce::Slider::LinearHorizontal);
            slider.setRange (0.0, 1.0);
            slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            slider.onDragStart = [this]
            {
                if (isAlive())
                    parameter->parameterChangeGestureBegin();
            };
            slider.onDragEnd = [this]
            {
                if (isAlive())
                    parameter->parameterChangeGestureEnd();
            };
            slider.onValueChange = [this]
            {
                if (isRefreshing || ! isAlive())
                    return;

                parameter->setNormalisedParameter ((float) slider.getValue(), juce::sendNotification);
            };

            for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &slider, &valueLabel })
                addAndMakeVisible (c);

            refresh();
        }

        // Polled rather than listened to: AutomatableParameter's listener is
        // async anyway, and a knob can also move from automation or the
        // plugin itself.
        void refresh()
        {
            if (! isAlive() || slider.isMouseButtonDown())
                return;

            const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
            slider.setValue (parameter->getCurrentNormalisedValue(), juce::dontSendNotification);
            valueLabel.setText (parameter->getCurrentValueAsStringWithLabel(), juce::dontSendNotification);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (6, 2);
            nameLabel.setBounds (area.removeFromLeft (100));
            valueLabel.setBounds (area.removeFromRight (64));
            slider.setBounds (area.reduced (4, 0));
        }

    private:
        te::AutomatableParameter::Ptr parameter;
        std::function<bool()> isAlive;

        juce::Label nameLabel, valueLabel;
        juce::Slider slider;
        bool isRefreshing = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Row)
    };

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
    std::vector<std::unique_ptr<Row>> rows;

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
          onClose (std::move (onCloseCallback))
    {
        auto panel = std::make_unique<EffectParameterPanel> (plugin);
        const auto contentHeight = juce::jlimit (60, maxHeight, panel->getHeight());

        viewport.setViewedComponent (panel.release(), true);
        viewport.setScrollBarsShown (true, false);
        viewport.setSize (EffectParameterPanel::width, contentHeight);

        setContentNonOwned (&viewport, true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (EffectParameterPanel::width, 60,
                         EffectParameterPanel::width, maxHeight);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();   // owner destroys this window (deferred)
    }

    void resized() override
    {
        juce::DocumentWindow::resized();

        // The panel is as tall as its parameter list; only the viewport
        // follows the window.
        if (auto* panel = viewport.getViewedComponent())
            panel->setSize (viewport.getMaximumVisibleWidth(), panel->getHeight());
    }

private:
    static constexpr int maxHeight = 460;

    std::function<void()> onClose;
    juce::Viewport viewport;
};

} // namespace carve::app
