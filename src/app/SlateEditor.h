#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "KnobPanel.h"
#include "PresetManager.h"

namespace te = tracktion;

namespace carve::app
{

// The editor for **Slate**, the built-in synth. Slate is the engine's own
// FourOscPlugin -- band-limited oscillators, per-voice filter, smoothed
// parameters, an exponential amp envelope -- with a front panel of our own,
// which is the whole of what "Slate" is. Without an editor a 4OSC generator
// could only ever be played at its defaults: an internal tracktion plugin
// brings no UI of its own and is not a juce::AudioProcessor either.
//
// One panel, two bands, top to bottom by how often they are reached for:
//
//   OSC 1 | OSC 2 | FILTER | AMP | OUT     the signal path
//   VOICE | LFO                            voice handling and modulation
//
// Within a section the knob size says the same thing: the oscillator's level,
// the filter cutoff and the output level are the big ones, the pulse width and
// the filter envelope's release are small. Everything is in view at once --
// dialling in a sound means moving between osc, filter and amp, and paging
// between them turned that into three views of one instrument.
//
// What the panel leaves out, it leaves out on purpose:
//
//   - Oscillators 3 and 4. Two are enough for the detuned-pair and
//     octave-stack sounds that are the point of having more than one, and
//     four made a panel you had to hunt through for the two anyone reaches
//     for. A preset may still use them.
//   - LFO 2 and the two modulation envelopes. 4OSC routes every modulator
//     through a matrix this panel deliberately does not show, and knobs that
//     cannot be pointed at anything are worse than absent ones. LFO 1 is here
//     because its destination box fills that matrix in for you.
//   - The built-in distortion, chorus, delay and reverb. The mixer's own
//     effect chain does all four, on any generator rather than just this one.
//
// Not every control is an AutomatableParameter: the wave shapes, the filter
// type and the sync switch are plain properties of the plugin's ValueTree, and
// the LFO's destination is a subtree of it. That tree is the patch either way,
// so every control writes to the same place, and EditSync copies the lot into
// the song on save.
class SlateEditor : public juce::Component,
                    private juce::Timer
{
public:
    explicit SlateEditor (te::FourOscPlugin&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    te::SafeSelectable<te::Plugin> plugin;

    PresetBar presetBar;
    KnobPage page;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlateEditor)
};

} // namespace carve::app
