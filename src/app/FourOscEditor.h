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

// A page of the 4OSC editor: titled sections laid out in a grid, row by row.
class EditorSectionPage : public juce::Component
{
public:
    explicit EditorSectionPage (int numColumns) : columns (numColumns) {}

    EditorSection& add (std::unique_ptr<EditorSection> section);

    void refresh();
    int getPreferredHeight() const;

    void resized() override;

private:
    int columns;
    std::vector<std::unique_ptr<EditorSection>> sections;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditorSectionPage)
};

//==============================================================================
// The editor for the built-in synth. Without it a 4OSC generator can only ever
// be played at its defaults, since an internal tracktion plugin brings no UI of
// its own and is not a juce::AudioProcessor either.
//
// The controls are grouped the way the plugin is, over three pages: the signal
// path (two oscillators, the filter and its envelope, the amp envelope and
// voice settings), the modulation sources, and the four built-in effects. As
// one flat list of seventy-odd rows -- which is what a generic parameter
// editor gives you -- nothing says which oscillator a "Tune" belongs to.
//
// Only two of the plugin's four oscillators are shown. The other two are still
// there and a preset may well use them; they are simply not what the editor
// puts in front of you.
//
// Not every control here is an AutomatableParameter: the wave shapes, the
// filter type and the effect on/off switches are plain properties of the
// plugin's ValueTree. That tree is the parameter store either way, so both
// kinds of row write to the same place, and EditSync copies the lot into the
// song on save.
class FourOscEditor : public juce::Component,
                      private juce::Timer
{
public:
    explicit FourOscEditor (te::FourOscPlugin&);

    // Three columns of sections at the row height the labels now need. The
    // window scrolls, so this is what the editor asks for rather than a limit.
    static constexpr int width = 760;
    static constexpr int height = 540;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    EditorSectionPage& addPage (const juce::String& name, int columns);

    te::SafeSelectable<te::Plugin> plugin;

    PresetBar presetBar;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    std::vector<EditorSectionPage*> pages;   // owned by the tabs

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FourOscEditor)
};


} // namespace carve::app
