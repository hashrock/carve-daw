#pragma once

#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::app
{

// The app's settings: where the sound comes out, and which MIDI keyboards it
// listens to.
//
// Both lists are the engine's own -- the audio half is JUCE's device selector
// pointed at the AudioDeviceManager tracktion already owns, which saves and
// reloads its own settings, and the MIDI half switches the engine's input
// devices on and off. There is nothing to apply and nothing to remember here:
// every control writes through to the thing it names.
//
// The audio selector is built with no input channels at all, and that is not a
// simplification: opening an audio input is what this app deliberately does
// not do (see the note in EngineSetup.h about JUCE 8.0.6 and CoreAudio). MIDI
// input is a different driver and is offered in full.
//
// Sized to its content -- put it in a window of preferredWidth x preferredHeight.
class SettingsComponent : public juce::Component,
                          private juce::ChangeListener
{
public:
    explicit SettingsComponent (te::Engine&);
    ~SettingsComponent() override;

    static constexpr int preferredWidth = 560;
    static constexpr int preferredHeight = 460;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // The engine's device list changed under us: a keyboard was plugged in, or
    // something else in the app switched one on.
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void rebuildMidiInputs();

    te::Engine& engine;

    juce::Label audioLabel, midiLabel, midiHintLabel;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> audioSelector;

    // One row per physical MIDI input. Held in their own component so the list
    // can scroll when the machine has more of them than there is room for.
    struct MidiRow
    {
        juce::String deviceID;
        std::unique_ptr<juce::ToggleButton> button;
    };

    juce::Component midiList;
    juce::Viewport midiViewport;
    std::vector<MidiRow> midiRows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsComponent)
};

} // namespace carve::app
