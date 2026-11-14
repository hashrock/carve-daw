#pragma once

#include <array>
#include <atomic>

#include <tracktion_engine/tracktion_engine.h>

#include "BassModel.h"

namespace te = tracktion;

namespace carve::plugins
{

// An electric bass, as an internal instrument, modelled rather than recorded
// (BassModel.h): four strings, the right hand that plays them, the fretting
// hand, the pickups and an amp, computed a sample at a time. Nothing to load.
// (The Electric Bass beside it is the other way round: a recording, analysed
// and resynthesised.)
//
// E1 to E4. One note per string, as on the instrument. A note that starts
// while another is held is played, as the Legato setting says, by plucking it
// (Retrigger), by hammering on or pulling off on the held note's string, or by
// sliding there; letting the later note go while the earlier one is still
// held goes back to it (a trill). Notes that start together -- a chord, a
// double stop -- are always plucked.
class BassModelPlugin : public te::Plugin
{
public:
    explicit BassModelPlugin (te::PluginCreationInfo);
    ~BassModelPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Modelled Bass"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Modelled Bass"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Bass"; }
    juce::String getSelectableDescription() override  { return TRANS ("Modelled Bass"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return false; }
    bool isSynth() override                           { return true; }
    bool producesAudioWhenNoAudioInput() override     { return true; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    // The factory sounds (BassModel.h). Setting one moves every knob, not undoably,
    // like any parameter move in the app.
    void setFactoryPreset (int index);
    int getMatchingFactoryPreset() const;   // -1 if the knobs are not one

    juce::CachedValue<float> styleValue, pluckValue, pickupValue, heightValue, toneValue, actionValue,
                             sustainValue, brightValue, muteValue, slideValue, fretlessValue, legatoValue,
                             driveValue, ampValue, volumeValue;

    te::AutomatableParameter::Ptr style, pluck, pickup, height, tone, action,
                                  sustain, bright, mute, slide, fretless, legato,
                                  drive, amp, volume;

    // For the editor, from the audio thread: each string's stopped fret, the
    // note held on it (-1 for none), and its shape.
    struct StringState
    {
        std::atomic<int> fret { 0 }, held { -1 };
        std::array<std::atomic<float>, 32> shape {};
    };

    const StringState& getStringState (int string) const  { return stringStates[(size_t) juce::jlimit (0, 3, string)]; }

private:
    bassmodel::Options readOptions() const;
    void startNote (int note, float velocity);
    void endNote (int note);

    bassmodel::Bass model;
    std::array<StringState, bassmodel::numStrings> stringStates;

    // Audio thread only: the keys down, in the order they went down, and when.
    struct Key { int note = 0; float velocity = 0.0f; std::int64_t at = 0; };
    std::array<Key, 16> keys {};
    int numKeys = 0;
    std::int64_t samplePosition = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassModelPlugin)
};

} // namespace carve::plugins
