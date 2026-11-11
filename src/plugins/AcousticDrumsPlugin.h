#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "GainReduction.h"
#include "InstrumentFx.h"

namespace ctd
{
struct Kit;
struct Alias;
struct Instrument;
struct Hit;
struct RenderOptions;
class HitVoice;
} // namespace ctd

namespace te = tracktion;

namespace carve::plugins
{

// A recorded acoustic kit, as an internal instrument -- without the samples.
//
// Every hit of the kit (26 instruments, 333 hits, from a CC0 multi-sampled
// kit) was analysed into three parts that are put back together as it plays:
//
//   attack  the first 15-160ms of the recording that the other two do not
//           explain, kept as it was (PCM);
//   body    the drum's resonant modes, as decaying sines;
//   noise   what is left -- snare wires, cymbal wash -- as a time-varying
//           spectral envelope played through a random-phase inverse FFT.
//
// So the whole kit is 1.7MB, every hit is a fresh waveform (the noise is
// never the same twice), and each of the three parts has a knob of its own.
// The engine is CtdDecoder.h, a port of the prototype's decoder, which runs
// at the kit's own 48kHz; at any other rate the output is resampled.
//
// Notes follow the General MIDI kit where it has the drum (see getPads); the
// hats share a choke group, so a closed hat stops an open one.
//
// The per-drum knobs live in the plugin's tree as plain properties, not as
// automatable parameters: twenty-eight drums with seven knobs each would bury
// every other automation target, and none of them is the kind of thing that
// moves within a song. The editor writes them, the audio thread reads a copy.
class AcousticDrumsPlugin : public te::Plugin,
                            public GainReductionSource
{
public:
    explicit AcousticDrumsPlugin (te::PluginCreationInfo);
    ~AcousticDrumsPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Acoustic Drums"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Acoustic Drums"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Drums"; }
    juce::String getSelectableDescription() override  { return TRANS ("Acoustic Drums"); }

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
    enum class Group { kickSnare, hats, toms, cymbals, percussion };

    struct Pad
    {
        const char* id;          // the kit's instrument id (or alias id)
        const char* name;
        const char* shortName;   // what fits on a pad
        std::array<int, 2> notes;   // the second is 0 when there is only one
        float mix;               // the kit's balance against itself
        Group group;
    };

    static constexpr int numPads = 28;
    static const std::array<Pad, numPads>& getPads();
    static const char* getGroupName (Group);

    // The pad a note plays, or -1.
    static int getPadForNote (int note);

    // The knobs every drum has.
    enum class Param { level, tune, decay, attack, body, noise, tone };
    static constexpr int numParams = 7;

    struct ParamSpec
    {
        const char* key;
        const char* name;
        float minimum, maximum, defaultValue;
        const char* unit;
    };

    static const ParamSpec& getParamSpec (Param);
    static juce::String formatValue (Param, float value);

    float getValue (int pad, Param) const;
    void setValue (int pad, Param, float value);   // not undoable, as every knob in the app
    void resetPad (int pad);

    // For the editor's drawings: the kit (null if it failed to load), and
    // what the decoder would be handed for this pad as the knobs stand.
    static const ctd::Kit* getKit();
    const ctd::Instrument* getInstrument (int pad) const;
    ctd::RenderOptions getRenderOptions (int pad) const;

    // A hit counter per pad, for the pads to flash from (see
    // DrumSynthPlugin::DrumActivity for why a counter).
    std::uint32_t getHitCount (int pad) const  { return hitCounts[(size_t) pad].load (std::memory_order_relaxed); }

    //==============================================================================
    // The kit's bus compressor, after everything, off until switched on.
    // Unlike the drums' knobs these are automatable: there are six, and a
    // compressor opening up for a chorus is the kind of thing a song does.
    juce::CachedValue<float> compThresholdValue, compRatioValue, compAttackValue, compReleaseValue,
                             compMakeupValue, compMixValue;
    te::AutomatableParameter::Ptr compThreshold, compRatio, compAttack, compRelease, compMakeup, compMix;

    bool isCompressorOn() const        { return compOn.load (std::memory_order_relaxed); }
    void setCompressorOn (bool);
    fx::BusCompressor::Settings getCompressorSettings() const;

    float getGainReductionDb() const noexcept override  { return isCompressorOn() ? compressor.getReductionDb() : 0.0f; }
    const fx::LevelHistory& getCompressorHistory() const  { return compressorHistory; }

private:
    struct Slot;
    struct ParamWatcher : juce::ValueTree::Listener
    {
        explicit ParamWatcher (AcousticDrumsPlugin& p) : owner (p) {}
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { owner.readValuesFromState(); }
        AcousticDrumsPlugin& owner;
    };

    void readValuesFromState();
    void trigger (int pad, float velocity, bool fullQuality);
    void renderVoices (float* out, int numSamples);
    void killAll();

    static juce::Identifier getPropertyId (int pad, Param);

    std::array<std::array<std::atomic<float>, numParams>, numPads> values;
    std::array<std::atomic<std::uint32_t>, numPads> hitCounts;

    // Audio thread only.
    std::vector<std::unique_ptr<Slot>> slots;
    std::array<std::uint32_t, numPads> roundRobin {};
    std::uint32_t noteSeed = 0;
    std::uint64_t age = 0;

    struct Bridge;
    std::unique_ptr<Bridge> bridge;

    juce::CachedValue<bool> compOnValue;
    std::atomic<bool> compOn { false };
    fx::BusCompressor compressor;
    fx::LevelHistory compressorHistory;

    ParamWatcher watcher { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AcousticDrumsPlugin)
};

} // namespace carve::plugins
