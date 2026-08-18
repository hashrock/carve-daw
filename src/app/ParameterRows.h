#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::app
{

// The building blocks of a generated plugin editor: a labelled row carrying one
// control. Internal tracktion plugins are not juce::AudioProcessors, so there
// is no GenericAudioProcessorEditor to fall back on -- every editor the app has
// for them is assembled out of these.
//
// Two things back a row. An AutomatableParameter has a range, a taper and a
// value-to-text function, so a normalised slider is enough. The rest of a
// plugin's settings -- wave shapes, filter type, effect on/off -- are plain
// CachedValues over the same ValueTree, and are edited by writing the property:
// the tree *is* the parameter store, so that is exactly what the plugin's own
// UI would do.
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
        nameLabel.setFont (juce::FontOptions (11.0f));
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8dc));
        addAndMakeVisible (nameLabel);
    }

    static constexpr int height = 22;

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
        valueLabel.setFont (juce::FontOptions (11.0f));
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

//==============================================================================
// A plugin-state property that is not an AutomatableParameter, shown as a combo
// box. `values` are the property values behind the menu entries, so a setting
// stored as 12/24 rather than 0/1 still reads naturally.
//
// `defaultValue` is what the *plugin* falls back to while the property is
// absent from the tree -- a freshly made synth has none of them yet. Reading
// the raw property in that state showed 0 ("Off") for settings the engine
// actually defaults to something else, like osc 1's sine.
class PropertyChoiceRow : public EditorRow
{
public:
    PropertyChoiceRow (juce::ValueTree stateToEdit, const juce::Identifier& propertyToEdit,
                       const juce::String& displayName,
                       const juce::StringArray& choiceNames, juce::Array<int> choiceValues,
                       std::function<bool()> isAliveCheck, int defaultValueToUse = 0)
        : EditorRow (displayName),
          state (std::move (stateToEdit)),
          property (propertyToEdit),
          values (std::move (choiceValues)),
          defaultValue (defaultValueToUse),
          isAlive (std::move (isAliveCheck))
    {
        combo.addItemList (choiceNames, 1);
        combo.onChange = [this]
        {
            if (isRefreshing || ! isAlive())
                return;

            const int index = combo.getSelectedItemIndex();

            if (juce::isPositiveAndBelow (index, values.size()))
                state.setProperty (property, values[index], nullptr);
        };

        addAndMakeVisible (combo);
        refresh();
    }

    // The common case: menu entry n stores the value n.
    PropertyChoiceRow (juce::ValueTree stateToEdit, const juce::Identifier& propertyToEdit,
                       const juce::String& displayName, const juce::StringArray& choiceNames,
                       std::function<bool()> isAliveCheck, int defaultValueToUse = 0)
        : PropertyChoiceRow (std::move (stateToEdit), propertyToEdit, displayName, choiceNames,
                             makeIndexValues (choiceNames.size()), std::move (isAliveCheck),
                             defaultValueToUse)
    {
    }

    void refresh() override
    {
        if (! isAlive())
            return;

        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
        combo.setSelectedItemIndex (
            juce::jmax (0, values.indexOf ((int) state.getProperty (property, defaultValue))),
            juce::dontSendNotification);
    }

protected:
    void layoutControls (juce::Rectangle<int> area) override
    {
        combo.setBounds (area.reduced (4, 1));
    }

private:
    static juce::Array<int> makeIndexValues (int count)
    {
        juce::Array<int> result;

        for (int i = 0; i < count; ++i)
            result.add (i);

        return result;
    }

    juce::ValueTree state;
    juce::Identifier property;
    juce::Array<int> values;
    int defaultValue = 0;
    std::function<bool()> isAlive;

    juce::ComboBox combo;
    bool isRefreshing = false;
};

//==============================================================================
// A bool plugin-state property as a tick box -- the effect on/off switches.
class PropertyToggleRow : public EditorRow
{
public:
    PropertyToggleRow (juce::ValueTree stateToEdit, const juce::Identifier& propertyToEdit,
                       const juce::String& displayName, std::function<bool()> isAliveCheck,
                       bool defaultValueToUse = false)
        : EditorRow (displayName),
          state (std::move (stateToEdit)),
          property (propertyToEdit),
          defaultValue (defaultValueToUse),
          isAlive (std::move (isAliveCheck))
    {
        toggle.onClick = [this]
        {
            if (isRefreshing || ! isAlive())
                return;

            state.setProperty (property, toggle.getToggleState(), nullptr);
        };

        addAndMakeVisible (toggle);
        refresh();
    }

    void refresh() override
    {
        if (! isAlive())
            return;

        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
        toggle.setToggleState ((bool) state.getProperty (property, defaultValue),
                               juce::dontSendNotification);
    }

protected:
    void layoutControls (juce::Rectangle<int> area) override
    {
        toggle.setBounds (area.removeFromLeft (24).reduced (0, 1));
    }

private:
    juce::ValueTree state;
    juce::Identifier property;
    bool defaultValue = false;
    std::function<bool()> isAlive;

    juce::ToggleButton toggle;
    bool isRefreshing = false;
};

//==============================================================================
// A numeric plugin-state property as a slider. Unlike a parameter this carries
// no range of its own, so the caller supplies one.
class PropertySliderRow : public EditorRow
{
public:
    PropertySliderRow (juce::ValueTree stateToEdit, const juce::Identifier& propertyToEdit,
                       const juce::String& displayName,
                       double minimum, double maximum, double interval,
                       const juce::String& valueSuffix,
                       std::function<bool()> isAliveCheck, double defaultValueToUse = 0.0)
        : EditorRow (displayName),
          state (std::move (stateToEdit)),
          property (propertyToEdit),
          defaultValue (defaultValueToUse),
          isAlive (std::move (isAliveCheck))
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setRange (minimum, maximum, interval);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setTextValueSuffix (valueSuffix);
        slider.onValueChange = [this]
        {
            if (isRefreshing || ! isAlive())
                return;

            state.setProperty (property, slider.getValue(), nullptr);
            valueLabel.setText (slider.getTextFromValue (slider.getValue()), juce::dontSendNotification);
        };

        valueLabel.setFont (juce::FontOptions (11.0f));
        valueLabel.setJustificationType (juce::Justification::centredRight);
        valueLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));

        addAndMakeVisible (slider);
        addAndMakeVisible (valueLabel);

        refresh();
    }

    void refresh() override
    {
        if (! isAlive() || slider.isMouseButtonDown())
            return;

        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
        slider.setValue ((double) state.getProperty (property, defaultValue),
                         juce::dontSendNotification);
        valueLabel.setText (slider.getTextFromValue (slider.getValue()), juce::dontSendNotification);
    }

protected:
    void layoutControls (juce::Rectangle<int> area) override
    {
        valueLabel.setBounds (area.removeFromRight (64));
        slider.setBounds (area.reduced (4, 0));
    }

private:
    juce::ValueTree state;
    juce::Identifier property;
    double defaultValue = 0.0;
    std::function<bool()> isAlive;

    juce::Label valueLabel;
    juce::Slider slider;
    bool isRefreshing = false;
};

//==============================================================================
// A titled box of rows. The 4OSC editor is built out of these: laid out as one
// flat list its 70-odd controls say nothing about which oscillator or which
// effect they belong to.
class EditorSection : public juce::Component
{
public:
    EditorSection (const juce::String& sectionTitle, int labelWidth)
        : title (sectionTitle), nameWidth (labelWidth)
    {
    }

    void add (std::unique_ptr<EditorRow> row)
    {
        row->setNameWidth (nameWidth);
        addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }

    void refresh()
    {
        for (auto& row : rows)
            row->refresh();
    }

    int getPreferredHeight() const
    {
        return titleHeight + (int) rows.size() * EditorRow::height + 4;
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff2a2a31));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);

        g.setColour (juce::Colour (0xffe0a24f));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (title, getLocalBounds().removeFromTop (titleHeight).reduced (8, 0),
                    juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().withTrimmedTop (titleHeight).reduced (2, 0);

        for (auto& row : rows)
            row->setBounds (area.removeFromTop (EditorRow::height));
    }

private:
    static constexpr int titleHeight = 18;

    juce::String title;
    int nameWidth;
    std::vector<std::unique_ptr<EditorRow>> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditorSection)
};

} // namespace carve::app
