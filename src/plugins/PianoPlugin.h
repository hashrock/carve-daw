#pragma once

#include <atomic>
#include <cstdint>

#include <tracktion_engine/tracktion_engine.h>

#include "PianoModel.h"

namespace te = tracktion;

namespace carve::plugins
{

// An acoustic piano, as an internal instrument, modelled rather than sampled:
// hammers, strings, bridge, soundboard and room are computed a sample at a
// time (PianoModel.h). Nothing to load, and no two notes quite alike -- a
// string still ringing is struck again as it is, and the strings of the keys
// held down ring along with the ones played.
//
// A0 to C8, as on the instrument. The sustain pedal is down while the Pedal
// switch (a parameter) is on, or while a CC64 that reaches the plugin says so
// -- the app's MIDI input forwards notes only, so in practice the switch.
class PianoPlugin : public te::Plugin
{
public:
    explicit PianoPlugin (te::PluginCreationInfo);
    ~PianoPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Piano"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Piano"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Piano"; }
    juce::String getSelectableDescription() override  { return TRANS ("Piano"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return false; }
    bool isSynth() override                           { return true; }
    bool producesAudioWhenNoAudioInput() override     { return true; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    juce::CachedValue<float> hardnessValue, knockValue, detuneValue, sympatheticValue,
                             brightnessValue, boardValue, reverbValue, volumeValue, pedalValue;

    te::AutomatableParameter::Ptr hardness, knock, detune, sympathetic,
                                  brightness, board, reverb, volume, pedal;

    // For the editor.
    int getVoiceCount() const      { return voiceCount.load (std::memory_order_relaxed); }
    bool isPedalDown() const       { return pedalDown.load (std::memory_order_relaxed); }

    // Which keys are down, as a bit per note (the low and high 64).
    std::uint64_t getKeysDown (int half) const  { return keysDown[half & 1].load (std::memory_order_relaxed); }

private:
    piano::Options readOptions() const;
    void setKey (int note, bool down);

    piano::Piano model;

    std::atomic<int> voiceCount { 0 };
    std::atomic<bool> pedalDown { false };
    bool midiPedal = false;   // audio thread only
    std::atomic<std::uint64_t> keysDown[2] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoPlugin)
};

} // namespace carve::plugins
