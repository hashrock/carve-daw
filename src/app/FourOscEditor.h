#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "KnobPanel.h"
#include "PresetManager.h"

namespace te = tracktion;

namespace carve::app
{

// The editor for the built-in synth. Without it a 4OSC generator can only ever
// be played at its defaults, since an internal tracktion plugin brings no UI of
// its own and is not a juce::AudioProcessor either.
//
// One panel, three bands, top to bottom by how often they are reached for:
//
//   OSC 1 | OSC 2 | FILTER | AMP | OUT       the signal path
//   VOICE | LFO 1 | LFO 2 | ENV 1 | ENV 2    modulation and voice handling
//   DIST | CHORUS | DELAY | REVERB           the built-in effects
//
// Within a section the knob size says the same thing: the oscillator's level,
// the filter cutoff and the output level are the big ones, the pulse width and
// the filter envelope's release are small. Everything is in view at once --
// dialling in a sound means moving between osc, filter and amp, and paging
// between them turned that into three views of one instrument.
//
// Only two of the plugin's four oscillators are shown. The other two are still
// there and a preset may well use them; they are simply not what the editor
// puts in front of you.
//
// Not every control here is an AutomatableParameter: the wave shapes, the
// filter type and the effect on/off switches are plain properties of the
// plugin's ValueTree. That tree is the parameter store either way, so both
// kinds of control write to the same place, and EditSync copies the lot into
// the song on save.
class FourOscEditor : public juce::Component,
                      private juce::Timer
{
public:
    explicit FourOscEditor (te::FourOscPlugin&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    te::SafeSelectable<te::Plugin> plugin;

    PresetBar presetBar;
    KnobPage page;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FourOscEditor)
};

} // namespace carve::app
