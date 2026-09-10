#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "Fonts.h"
#include "ParameterRows.h"

namespace te = tracktion;

namespace carve::app
{

// The pieces of a knob-based plugin editor: a rotary knob for one value, a
// titled section holding rows of them, and a page that lays sections out in
// rows. The 4OSC and 808 editors are built out of these.
//
// Knobs rather than the labelled slider rows of ParameterRows.h because a
// synth has sixty-odd continuous values, and a row per value is a wall of
// text that scrolls off the window. A knob is a fifth of the width, and its
// *size* can say how much it matters: the master level and the filter cutoff
// are the big ones, the pulse width is not.
//
// A knob shows its name under itself and swaps that for the value while the
// mouse is over it, so the panel is not a grid of numbers that only matter
// one at a time.

enum class KnobSize { small, medium, large, xlarge };

//==============================================================================
// The rotary control itself: a juce::Slider that paints its own dial. A
// bipolar knob (pan, tune, an amount that goes both ways) fills its arc from
// the top rather than from the left, so the neutral position reads as such.
class RotaryDial : public juce::Slider
{
public:
    RotaryDial()
    {
        setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        setWantsKeyboardFocus (false);
        setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                             juce::MathConstants<float>::pi * 2.75f, true);
    }

    void setBipolar (bool shouldBeBipolar)
    {
        bipolar = shouldBeBipolar;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const auto params = getRotaryParameters();
        const auto proportion = (float) valueToProportionOfLength (getValue());
        const auto angle = params.startAngleRadians
                           + proportion * (params.endAngleRadians - params.startAngleRadians);

        const auto trackWidth = juce::jmax (2.0f, radius * 0.14f);
        const auto arcRadius = radius - trackWidth * 0.5f;
        const auto alpha = isEnabled() ? 1.0f : 0.4f;

        // The full sweep, dim, so the value arc has something to be a part of.
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                             params.startAngleRadians, params.endAngleRadians, true);
        g.setColour (juce::Colour (0xff3a3a40).withMultipliedAlpha (alpha));
        g.strokePath (track, juce::PathStrokeType (trackWidth, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));

        const auto zeroAngle = bipolar ? (params.startAngleRadians + params.endAngleRadians) * 0.5f
                                       : params.startAngleRadians;

        if (std::abs (angle - zeroAngle) > 0.001f)
        {
            juce::Path value;
            value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                 juce::jmin (zeroAngle, angle), juce::jmax (zeroAngle, angle), true);
            g.setColour (juce::Colour (0xffe0a24f).withMultipliedAlpha (alpha));
            g.strokePath (value, juce::PathStrokeType (trackWidth, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        }

        // The cap, and a pointer that is easier to read at a glance than the arc.
        const auto capRadius = radius - trackWidth * 1.8f;
        g.setColour (juce::Colour (0xff2c2c33).withMultipliedAlpha (alpha));
        g.fillEllipse (juce::Rectangle<float> (capRadius * 2.0f, capRadius * 2.0f).withCentre (centre));
        g.setColour (juce::Colour (0xff4a4a52).withMultipliedAlpha (alpha));
        g.drawEllipse (juce::Rectangle<float> (capRadius * 2.0f, capRadius * 2.0f).withCentre (centre), 1.0f);

        const auto pointerLength = capRadius * 0.55f;
        const auto pointerStart = centre.getPointOnCircumference (capRadius - pointerLength - 2.0f, angle);
        const auto pointerEnd = centre.getPointOnCircumference (capRadius - 2.0f, angle);
        g.setColour (juce::Colour (0xffe8e8ec).withMultipliedAlpha (alpha));
        g.drawLine (juce::Line<float> (pointerStart, pointerEnd), juce::jmax (1.5f, radius * 0.08f));
    }

private:
    bool bipolar = false;
};

//==============================================================================
// A dial with its name under it. Which value it edits is the subclass's
// business; this handles the size, the label and the value-on-hover.
class Knob : public juce::Component
{
public:
    Knob (const juce::String& knobName, KnobSize knobSize)
        : name (knobName), size (knobSize)
    {
        label.setText (name, juce::dontSendNotification);
        label.setFont (uiFont (labelFontHeight));
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colour (nameColour));
        label.setInterceptsMouseClicks (false, false);

        // Hover on the dial counts as hover on the knob: that is where the
        // value is shown.
        dial.addMouseListener (this, false);

        addAndMakeVisible (dial);
        addAndMakeVisible (label);
    }

    ~Knob() override
    {
        dial.removeMouseListener (this);
    }

    // Pulls the dial back into line with whatever it edits.
    virtual void refresh() = 0;

    static int getDiameter (KnobSize s)
    {
        switch (s)
        {
            case KnobSize::small:  return 26;
            case KnobSize::medium: return 36;
            case KnobSize::large:  return 48;
            case KnobSize::xlarge: return 64;
        }

        return 36;
    }

    // Wide enough for the dial and, if it is longer, the name.
    int getPreferredWidth() const
    {
        const auto textWidth = juce::GlyphArrangement::getStringWidthInt (
                                   juce::Font (uiFont (labelFontHeight)), name) + 6;
        return juce::jmax (getDiameter (size) + 14, textWidth);
    }

    int getPreferredHeight() const
    {
        return getDiameter (size) + labelHeight + 2;
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto labelArea = area.removeFromBottom (labelHeight);

        // A value string can be longer than the name the cell was sized for;
        // let it spill over the gaps either side rather than clip.
        label.setBounds (labelArea.expanded (8, 0));

        const int diameter = getDiameter (size);
        dial.setBounds (juce::Rectangle<int> (diameter, diameter).withCentre (area.getCentre()));
    }

    void mouseEnter (const juce::MouseEvent&) override  { hovering = true;  updateLabel(); }
    void mouseExit (const juce::MouseEvent&) override   { hovering = false; updateLabel(); }

protected:
    static constexpr float labelFontHeight = fonts::small;
    static constexpr int labelHeight = 15;

    // What to show in place of the name while the mouse is over the knob.
    virtual juce::String getValueText() = 0;

    void updateLabel()
    {
        const bool showValue = hovering || dial.isMouseButtonDown();
        label.setText (showValue ? getValueText() : name, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, juce::Colour (showValue ? valueColour : nameColour));
    }

    RotaryDial dial;

private:
    static constexpr juce::uint32 nameColour = 0xffb8b8c0;
    static constexpr juce::uint32 valueColour = 0xfff0f0f4;

    juce::String name;
    KnobSize size;
    juce::Label label;
    bool hovering = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Knob)
};

//==============================================================================
// One te::AutomatableParameter as a knob. Normalised: the parameter owns the
// taper, and its own value string is what gets displayed anyway.
//
// The parameter is held by Ptr and guarded by an "is the plugin still there?"
// callback: EditSync can delete the plugin under an open editor window, and
// the window is only closed afterwards.
class ParameterKnob : public Knob
{
public:
    ParameterKnob (te::AutomatableParameter& parameterToEdit, const juce::String& displayName,
                   KnobSize knobSize, std::function<bool()> isAliveCheck)
        : Knob (displayName, knobSize),
          parameter (&parameterToEdit),
          isAlive (std::move (isAliveCheck))
    {
        dial.setRange (0.0, 1.0);

        const auto range = parameterToEdit.getValueRange();
        dial.setBipolar (range.getStart() < 0.0f && range.getEnd() > 0.0f);

        if (const auto defaultValue = parameterToEdit.getDefaultValue())
            dial.setDoubleClickReturnValue (true, parameterToEdit.valueRange.convertTo0to1 (*defaultValue));

        dial.onDragStart = [this]
        {
            if (isAlive())
                parameter->parameterChangeGestureBegin();
        };
        dial.onDragEnd = [this]
        {
            if (isAlive())
                parameter->parameterChangeGestureEnd();

            updateLabel();
        };
        dial.onValueChange = [this]
        {
            if (isRefreshing || ! isAlive())
                return;

            parameter->setNormalisedParameter ((float) dial.getValue(), juce::sendNotification);
            updateLabel();
        };

        refresh();
    }

    void refresh() override
    {
        if (! isAlive() || dial.isMouseButtonDown())
            return;

        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
        dial.setValue (parameter->getCurrentNormalisedValue(), juce::dontSendNotification);

        if (dial.isMouseOver())
            updateLabel();
    }

protected:
    juce::String getValueText() override
    {
        return isAlive() ? parameter->getCurrentValueAsStringWithLabel() : juce::String();
    }

private:
    te::AutomatableParameter::Ptr parameter;
    std::function<bool()> isAlive;
    bool isRefreshing = false;
};

//==============================================================================
// A numeric plugin-state property as a knob. Unlike a parameter this carries
// no range of its own, so the caller supplies one.
class PropertyKnob : public Knob
{
public:
    PropertyKnob (juce::ValueTree stateToEdit, const juce::Identifier& propertyToEdit,
                  const juce::String& displayName, KnobSize knobSize,
                  double minimum, double maximum, double interval, const juce::String& valueSuffix,
                  std::function<bool()> isAliveCheck, double defaultValueToUse)
        : Knob (displayName, knobSize),
          state (std::move (stateToEdit)),
          property (propertyToEdit),
          defaultValue (defaultValueToUse),
          isAlive (std::move (isAliveCheck))
    {
        dial.setRange (minimum, maximum, interval);
        dial.setTextValueSuffix (valueSuffix);
        dial.setDoubleClickReturnValue (true, defaultValue);
        dial.onDragEnd = [this] { updateLabel(); };
        dial.onValueChange = [this]
        {
            if (isRefreshing || ! isAlive())
                return;

            state.setProperty (property, dial.getValue(), nullptr);
            updateLabel();
        };

        refresh();
    }

    void refresh() override
    {
        if (! isAlive() || dial.isMouseButtonDown())
            return;

        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
        dial.setValue ((double) state.getProperty (property, defaultValue), juce::dontSendNotification);

        if (dial.isMouseOver())
            updateLabel();
    }

protected:
    juce::String getValueText() override
    {
        return dial.getTextFromValue (dial.getValue());
    }

private:
    juce::ValueTree state;
    juce::Identifier property;
    double defaultValue = 0.0;
    std::function<bool()> isAlive;
    bool isRefreshing = false;
};

//==============================================================================
// The controls a section's title band can hold, for the settings that are
// not continuous: a plain-property menu and a plain-property switch. No name
// label -- "Sine", "Low Pass", "Poly" say what they are, and the band is
// narrow.
class HeaderControl
{
public:
    virtual ~HeaderControl() = default;

    virtual juce::Component& getComponent() = 0;
    virtual void refresh() = 0;
    virtual int getPreferredWidth() const = 0;
};

// `values` are the property values behind the menu entries, so a setting
// stored as 12/24 rather than 0/1 still reads naturally. `defaultValue` is what
// the *plugin* falls back to while the property is absent from the tree -- a
// freshly made synth has none of them yet, and reading the raw property in
// that state showed 0 ("Off") for settings the engine defaults to something
// else, like osc 1's sine.
class PropertyChoiceBox : public juce::ComboBox,
                          public HeaderControl
{
public:
    PropertyChoiceBox (juce::ValueTree stateToEdit, const juce::Identifier& propertyToEdit,
                       const juce::StringArray& choiceNames, juce::Array<int> choiceValues,
                       std::function<bool()> isAliveCheck, int defaultValueToUse)
        : state (std::move (stateToEdit)),
          property (propertyToEdit),
          values (std::move (choiceValues)),
          defaultValue (defaultValueToUse),
          isAlive (std::move (isAliveCheck))
    {
        addItemList (choiceNames, 1);
        setWantsKeyboardFocus (false);

        // Wide enough for the longest entry beside the arrow.
        for (const auto& name : choiceNames)
            width = juce::jmax (width, juce::GlyphArrangement::getStringWidthInt (
                                           juce::Font (uiFont (EditorRow::rowFontHeight)), name) + 46);

        onChange = [this]
        {
            if (isRefreshing || ! isAlive())
                return;

            const int index = getSelectedItemIndex();

            if (juce::isPositiveAndBelow (index, values.size()))
                state.setProperty (property, values[index], nullptr);
        };

        refresh();
    }

    PropertyChoiceBox (juce::ValueTree stateToEdit, const juce::Identifier& propertyToEdit,
                       const juce::StringArray& choiceNames,
                       std::function<bool()> isAliveCheck, int defaultValueToUse = 0)
        : PropertyChoiceBox (std::move (stateToEdit), propertyToEdit, choiceNames,
                             makeIndexValues (choiceNames.size()), std::move (isAliveCheck),
                             defaultValueToUse)
    {
    }

    juce::Component& getComponent() override  { return *this; }
    int getPreferredWidth() const override    { return width; }

    void refresh() override
    {
        if (! isAlive())
            return;

        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
        setSelectedItemIndex (juce::jmax (0, values.indexOf ((int) state.getProperty (property, defaultValue))),
                              juce::dontSendNotification);
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
    int width = 60;
    bool isRefreshing = false;
};

class PropertyToggleButton : public juce::ToggleButton,
                             public HeaderControl
{
public:
    PropertyToggleButton (juce::ValueTree stateToEdit, const juce::Identifier& propertyToEdit,
                          const juce::String& text, std::function<bool()> isAliveCheck,
                          bool defaultValueToUse = false)
        : juce::ToggleButton (text),
          state (std::move (stateToEdit)),
          property (propertyToEdit),
          defaultValue (defaultValueToUse),
          isAlive (std::move (isAliveCheck))
    {
        setWantsKeyboardFocus (false);
        width = juce::GlyphArrangement::getStringWidthInt (
                    juce::Font (uiFont (EditorRow::rowFontHeight)), text) + 30;

        onClick = [this]
        {
            if (isRefreshing || ! isAlive())
                return;

            state.setProperty (property, getToggleState(), nullptr);
        };

        refresh();
    }

    juce::Component& getComponent() override  { return *this; }
    int getPreferredWidth() const override    { return width; }

    void refresh() override
    {
        if (! isAlive())
            return;

        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
        setToggleState ((bool) state.getProperty (property, defaultValue), juce::dontSendNotification);
    }

private:
    juce::ValueTree state;
    juce::Identifier property;
    bool defaultValue = false;
    std::function<bool()> isAlive;
    int width = 40;
    bool isRefreshing = false;
};

//==============================================================================
// A titled box of knobs, in rows, with room in the title band for the few
// settings that are not knobs. Rows are left-aligned by default; a drum's
// column of knobs reads better centred.
class KnobSection : public juce::Component
{
public:
    explicit KnobSection (const juce::String& sectionTitle)
        : title (sectionTitle)
    {
    }

    // A control for the title band, after the title.
    void addHeaderControl (std::unique_ptr<HeaderControl> control)
    {
        addAndMakeVisible (control->getComponent());
        headerControls.push_back (std::move (control));
    }

    Knob& addKnob (std::unique_ptr<Knob> knob)
    {
        if (rows.empty())
            rows.emplace_back();

        auto& added = *knob;
        addAndMakeVisible (added);
        rows.back().push_back (std::move (knob));
        return added;
    }

    void newRow()
    {
        rows.emplace_back();
    }

    void setCentred (bool shouldCentreRows)  { centred = shouldCentreRows; }
    void setDrawsBackground (bool should)     { drawsBackground = should; }

    void refresh()
    {
        for (auto& control : headerControls)
            control->refresh();

        for (auto& row : rows)
            for (auto& knob : row)
                knob->refresh();
    }

    int getPreferredWidth() const
    {
        int width = 0;

        if (hasHeader())
        {
            int header = title.isEmpty() ? 0 : getTitleWidth() + headerGap;

            for (auto& control : headerControls)
                header += control->getPreferredWidth() + headerGap;

            width = header - headerGap;
        }

        for (auto& row : rows)
        {
            int rowWidth = 0;

            for (auto& knob : row)
                rowWidth += knob->getPreferredWidth() + knobGap;

            width = juce::jmax (width, rowWidth - knobGap);
        }

        return width + padding * 2;
    }

    int getPreferredHeight() const
    {
        int height = (hasHeader() ? headerHeight : 0) + padding;

        for (auto& row : rows)
        {
            int rowHeight = 0;

            for (auto& knob : row)
                rowHeight = juce::jmax (rowHeight, knob->getPreferredHeight());

            height += rowHeight + rowGap;
        }

        return height - rowGap + padding;
    }

    void paint (juce::Graphics& g) override
    {
        if (drawsBackground)
        {
            g.setColour (juce::Colour (0xff2a2a31));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);
        }

        if (title.isNotEmpty())
        {
            g.setColour (juce::Colour (0xffe0a24f));
            g.setFont (getTitleFont());
            g.drawText (title, getLocalBounds().removeFromTop (headerHeight).reduced (padding + 2, 0),
                        juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (padding);

        if (hasHeader())
        {
            auto header = area.removeFromTop (headerHeight);

            if (title.isNotEmpty())
                header.removeFromLeft (getTitleWidth() + headerGap);

            for (auto& control : headerControls)
            {
                const int w = control->getPreferredWidth();
                control->getComponent().setBounds (header.removeFromLeft (w).withSizeKeepingCentre (w, headerControlHeight));
                header.removeFromLeft (headerGap);
            }
        }

        for (auto& row : rows)
        {
            int rowWidth = 0, rowHeight = 0;

            for (auto& knob : row)
            {
                rowWidth += knob->getPreferredWidth() + knobGap;
                rowHeight = juce::jmax (rowHeight, knob->getPreferredHeight());
            }

            auto rowArea = area.removeFromTop (rowHeight);

            if (centred)
                rowArea = rowArea.withSizeKeepingCentre (rowWidth - knobGap, rowHeight);

            for (auto& knob : row)
            {
                const int w = knob->getPreferredWidth();
                // Bottom-aligned, so a row of mixed sizes keeps its labels on one line.
                knob->setBounds (rowArea.removeFromLeft (w).withTrimmedTop (rowHeight - knob->getPreferredHeight()));
                rowArea.removeFromLeft (knobGap);
            }

            area.removeFromTop (rowGap);
        }
    }

private:
    static constexpr int headerHeight = 26;
    static constexpr int headerControlHeight = 22;
    static constexpr int headerGap = 6;
    static constexpr int padding = 6;
    static constexpr int rowGap = 4;
    static constexpr int knobGap = 2;

    bool hasHeader() const  { return title.isNotEmpty() || ! headerControls.empty(); }

    static juce::Font getTitleFont()
    {
        return juce::Font (uiFont (EditorRow::rowFontHeight, juce::Font::bold));
    }

    int getTitleWidth() const
    {
        return juce::GlyphArrangement::getStringWidthInt (getTitleFont(), title) + 4;
    }

    juce::String title;
    std::vector<std::unique_ptr<HeaderControl>> headerControls;
    std::vector<std::vector<std::unique_ptr<Knob>>> rows;
    bool centred = false;
    bool drawsBackground = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KnobSection)
};

//==============================================================================
// Sections laid out in rows. Each section gets its preferred width plus an
// equal share of whatever the row has left over, and the height of the
// tallest section in its row, so a row's boxes line up along both edges.
class KnobPage : public juce::Component
{
public:
    KnobPage() = default;

    KnobSection& add (std::unique_ptr<KnobSection> section)
    {
        if (rows.empty())
            rows.emplace_back();

        auto& added = *section;
        addAndMakeVisible (added);
        rows.back().push_back (std::move (section));
        return added;
    }

    void newRow()
    {
        rows.emplace_back();
    }

    void refresh()
    {
        for (auto& row : rows)
            for (auto& section : row)
                section->refresh();
    }

    int getPreferredWidth() const
    {
        int width = 0;

        for (auto& row : rows)
        {
            int rowWidth = 0;

            for (auto& section : row)
                rowWidth += section->getPreferredWidth() + gap;

            width = juce::jmax (width, rowWidth - gap);
        }

        return width + margin * 2;
    }

    int getPreferredHeight() const
    {
        int height = margin;

        for (auto& row : rows)
            height += getRowHeight (row) + gap;

        return height - gap + margin;
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (margin);

        for (auto& row : rows)
        {
            if (row.empty())
                continue;

            auto rowArea = area.removeFromTop (getRowHeight (row));
            area.removeFromTop (gap);

            int preferred = 0;

            for (auto& section : row)
                preferred += section->getPreferredWidth() + gap;

            const int extra = juce::jmax (0, rowArea.getWidth() - (preferred - gap)) / (int) row.size();

            for (auto& section : row)
            {
                section->setBounds (rowArea.removeFromLeft (section->getPreferredWidth() + extra));
                rowArea.removeFromLeft (gap);
            }
        }
    }

private:
    static constexpr int margin = 6;
    static constexpr int gap = 6;

    static int getRowHeight (const std::vector<std::unique_ptr<KnobSection>>& row)
    {
        int height = 0;

        for (auto& section : row)
            height = juce::jmax (height, section->getPreferredHeight());

        return height;
    }

    std::vector<std::vector<std::unique_ptr<KnobSection>>> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KnobPage)
};

} // namespace carve::app
