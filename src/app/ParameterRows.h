#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "Fonts.h"

namespace te = tracktion;

namespace carve::app
{

// The building block of a generated effect editor: a labelled row carrying one
// slider. Internal tracktion plugins are not juce::AudioProcessors, so there
// is no GenericAudioProcessorEditor to fall back on -- the effect windows are
// assembled out of these (the synth editors use the knobs in KnobPanel.h).
//
// An AutomatableParameter has a range, a taper and a value-to-text function,
// so a normalised slider is enough.
//
// Rows are polled rather than listened to. AutomatableParameter's listener is
// async anyway, and a control can also move from automation, a preset load or
// the plugin itself.
class EditorRow : public juce::Component
{
public:
    explicit EditorRow (const juce::String& rowName)
    {
        nameLabel.setText (rowName, juce::dontSendNotification);
        nameLabel.setFont (uiFont (rowFontHeight));
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8dc));
        addAndMakeVisible (nameLabel);
    }

    static constexpr int height = 26;

    // Every label in a generated editor is this size. It was 11px, which is
    // small enough to have to lean in for -- and a plugin editor is a wall of
    // these, so the whole panel read as fine print.
    static constexpr float rowFontHeight = fonts::normal;

    // Pulls the control back into line with whatever it edits.
    virtual void refresh() = 0;

    void setNameWidth (int newWidth)
    {
        nameWidth = newWidth;
        resized();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (6, 2);
        nameLabel.setBounds (area.removeFromLeft (nameWidth));
        layoutControls (area);
    }

protected:
    virtual void layoutControls (juce::Rectangle<int> area) = 0;

    juce::Label nameLabel;
    int nameWidth = 100;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditorRow)
};

//==============================================================================
// One te::AutomatableParameter as a normalised slider plus its own value string.
//
// The parameter is held by Ptr and guarded by an "is the plugin still there?"
// callback: EditSync can delete the plugin under an open editor window, and the
// window is only closed afterwards.
class ParameterSliderRow : public EditorRow
{
public:
    ParameterSliderRow (te::AutomatableParameter& parameterToEdit,
                        const juce::String& displayName,
                        std::function<bool()> isAliveCheck)
        : EditorRow (displayName),
          parameter (&parameterToEdit),
          isAlive (std::move (isAliveCheck))
    {
        valueLabel.setFont (uiFont (rowFontHeight));
        valueLabel.setJustificationType (juce::Justification::centredRight);
        valueLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));

        // Normalised: the parameter owns the taper, and its own value string is
        // what gets displayed anyway.
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

        addAndMakeVisible (slider);
        addAndMakeVisible (valueLabel);

        refresh();
    }

    ParameterSliderRow (te::AutomatableParameter& parameterToEdit, std::function<bool()> isAliveCheck)
        : ParameterSliderRow (parameterToEdit, parameterToEdit.getParameterName(), std::move (isAliveCheck))
    {
    }

    void refresh() override
    {
        if (! isAlive() || slider.isMouseButtonDown())
            return;

        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
        slider.setValue (parameter->getCurrentNormalisedValue(), juce::dontSendNotification);
        valueLabel.setText (parameter->getCurrentValueAsStringWithLabel(), juce::dontSendNotification);
    }

protected:
    void layoutControls (juce::Rectangle<int> area) override
    {
        valueLabel.setBounds (area.removeFromRight (valueWidth));
        slider.setBounds (area.reduced (4, 0));
    }

private:
    static constexpr int valueWidth = 64;

    te::AutomatableParameter::Ptr parameter;
    std::function<bool()> isAlive;

    juce::Label valueLabel;
    juce::Slider slider;
    bool isRefreshing = false;
};

} // namespace carve::app
