#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <thread>

#include <tracktion_engine/tracktion_engine.h>

#include "DrumModelKit.h"

namespace te = tracktion;

namespace carve::plugins
{

// A drum kit, as an internal instrument, modelled rather than recorded: the
// PhysKit project's physically modelled kit (DrumModel.h). The kick, the
// snare and the three toms are computed -- heads, air, shell, the stick or
// beater meeting the head, the snare wires rattling on the resonant head, the
// mics -- each stroke once, in the background, the first time it is played,
// and replayed after that (drummodel::Kit); a stroke not rendered yet is
// solved live. The cymbals are samples of the same kit
// rendered offline (their physics runs far slower than real time). (Acoustic
// Drums beside it is the other way round: a recording, analysed and
// resynthesised.)
//
// General MIDI notes (drummodel::pieceForNote); velocity sets the stick's
// speed. The hats and the crash choke as the kit's SFZ does.
//
// The models are compiled in, gzipped; they are unpacked, and the snare's
// wires settled onto its head, on a thread of their own when the plugin is
// made, and initialise() waits for that.
//
// The notes of its track's clips are the strokes the kit keeps ready: looked
// over twice a second, so a song plays from the cache from its first note
// (the strokes render in the background as soon as the song is open).
class DrumModelPlugin : public te::Plugin,
                        private juce::Timer
{
public:
    explicit DrumModelPlugin (te::PluginCreationInfo);
    ~DrumModelPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Modelled Drums"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Modelled Drums"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Drums"; }
    juce::String getSelectableDescription() override  { return TRANS ("Modelled Drums"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return false; }
    bool isSynth() override                           { return true; }
    bool producesAudioWhenNoAudioInput() override     { return true; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    //==============================================================================
    // The knobs, in the order of this table (and of the editor's sections).
    enum Param
    {
        kickTune, kickMute, kickGlide, kickBeater, kickLevel,
        snareTune, snareMute, snareWires, snareStick, snareLevel, snareRim,
        tom1Tune, tom1Mute, tom1Glide, tom1Stick,
        tom2Tune, tom2Mute, tom2Glide, tom2Stick,
        tom3Tune, tom3Mute, tom3Glide, tom3Stick,
        hatTune, hatLevel, crashTune, crashLevel, rideTune, rideLevel,
        close, overhead, room, volume,
        numParams
    };

    struct ParamInfo
    {
        const char* id;
        const char* name;
        float min, max, defaultValue;
    };

    static const ParamInfo& getParamInfo (int index);
    te::AutomatableParameter& getParam (Param p)  { return *params[(size_t) p]; }

    // The pieces and their notes, as the piano roll names its lanes.
    struct Note { int note; drummodel::Piece piece; };
    static const std::array<Note, 13>& getNotes();

    // For the editor, from the audio thread: how busy each drum is.
    struct Activity { std::atomic<int> kick { 0 }, snare { 0 }, toms[3] {}, cymbals { 0 }; };
    const Activity& getActivity() const  { return activity; }
    std::size_t getCacheBytes() const    { return isLoaded() ? kit->getCacheBytes() : 0; }   // the strokes it keeps (any thread)
    bool isLoaded() const                { return loadState.load() == 1; }
    bool failedToLoad() const            { return loadState.load() == 2; }

private:
    drummodel::Settings readSettings() const;
    void waitForLoad();
    void timerCallback() override;   // the strokes the track's clips play

    std::array<juce::CachedValue<float>, numParams> values;
    std::array<te::AutomatableParameter::Ptr, numParams> params;

    std::unique_ptr<drummodel::Kit> kit;
    std::thread loader;
    std::atomic<int> loadState { 0 };   // 0 loading, 1 loaded, 2 failed
    Activity activity;
    drummodel::Kit::Strokes keptStrokes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumModelPlugin)
};

} // namespace carve::plugins
