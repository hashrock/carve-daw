#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "voice/VocalModel.h"
#include "voice/VocalScore.h"

namespace te = tracktion;

namespace carve::plugins
{

// A singer computed rather than recorded: the physically modelled voice of
// src/voice/VocalModel.h, sung from the Japanese lyrics on a part's notes.
//
// A sung note starts before its beat (the consonant goes ahead of the vowel,
// and a phrase opens with a breath in), and how it is sung depends on the
// whole phrase around it: the scoops, the accents and the swell are chosen
// from the shape of the melody (autoExpress), and each mora carries on from
// the one before. None of that fits in a MIDI note-on. So, as with the voicebank
// Singer, EditSync hands the plugin a part's notes ahead of time and gets
// back, per note, an id and how far ahead of the note its first movement
// is; it puts a trigger -- a SysEx carrying the id -- into the clip that far
// ahead. The plugin works out the note's movements when it is prepared, on
// the message thread, and the trigger only copies them into the voice's
// fixed-length queue. A part is keyed by its clip and repeat and a note by
// its place in it, so an edit keeps the ids and replaces what they sing.
//
// The voice itself runs live, a sample at a time, on the audio thread:
// stopping, seeking or looping (an all-notes-off) lets it go quiet, and the
// next trigger takes it from there.
//
// Live notes -- a keyboard, the piano roll's previews -- are planned on the
// spot the way the prototype's keyboard is: the consonant delays the vowel
// (nothing can be planned ahead), a key held into the next joins them, and
// letting go fades the voice out. They sing the editor's live lyric, a mora
// per key, "あ" when there is none.
class VocalModelPlugin : public te::Plugin,
                         private juce::Timer
{
public:
    explicit VocalModelPlugin (te::PluginCreationInfo);
    ~VocalModelPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Modelled Voice"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Modelled Voice"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Voice"; }
    juce::String getSelectableDescription() override  { return TRANS ("Modelled Voice"); }

    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return false; }
    bool isSynth() override                           { return true; }
    bool producesAudioWhenNoAudioInput() override     { return true; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    //==========================================================================
    // The knobs, grouped as on the prototype's "voice design" panel.
    juce::CachedValue<float> tractLengthValue, foldValue, resonanceValue,          // body
                             breathValue, pressValue, registerValue,               // folds and breath
                             jitterValue, shimmerValue,
                             throatValue, pharynxValue, twangValue,                // throat
                             nasalValue,                                           // nose
                             clarityValue, vowelPresetValue,                       // mouth
                             vibratoValue, vibratoRateValue, driftValue,           // singing
                             brightnessValue, bodyValue, ringValue, airValue,      // mic and room
                             reverbValue, volumeValue,
                             strengthValue, speedValue;                            // expression, how quickly it moves

    te::AutomatableParameter::Ptr tractLength, fold, resonance, breath, press, registerMix, jitter, shimmer,
                                  throat, pharynx, twang,
                                  nasal, clarity, vowelPreset, vibrato, vibratoRate, drift,
                                  brightness, body, ring, air, reverb, volume, strength, speed;

    // What a live key sings (not automatable: text).
    juce::CachedValue<juce::String> liveLyric;

    // Sets every knob a character has, as one undoable change (message thread).
    void applyCharacter (int index);

    // Which character the knobs are at, or -1 when they are at none.
    int findCurrentCharacter() const;

    //==========================================================================
    // The sync's side (message thread).

    // How far ahead of each note its trigger goes, and the trigger itself.
    struct Prepared
    {
        int id = -1;            // -1: nothing to sing (the lyric reads as nothing)
        double leadSec = 0.0;
    };

    // One pass of EditSync over this generator's parts: begin, prepare each
    // part (a clip's notes, or one repeat of them), end. Parts not prepared in
    // a pass are let go of a little later.
    void beginPreparing();
    std::vector<Prepared> preparePart (const juce::String& partKey, const std::vector<voice::vocal::PartNote>& notes);
    void endPreparing();

    static juce::MidiMessage createTrigger (int id, double skipSec);
    static bool parseTrigger (const juce::MidiMessage&, int& id, double& skipSec);

    // For the editor.
    bool isVoiced() const       { return voicedNow.load (std::memory_order_relaxed); }
    float getSungHz() const     { return sungHz.load (std::memory_order_relaxed); }

    // A note's movements, relative to the note's start.
    struct Plan
    {
        std::vector<voice::vocal::Event> events;
        double leadSec = 0.0;   // where its trigger is, ahead of the note
    };

private:
    void timerCallback() override;
    void replanAll();
    double currentCenter() const;
    void retire (std::shared_ptr<const Plan>);
    void markUnwanted (int id, double now);
    void setLiveLyric (const juce::String&);
    voice::vocal::Options readOptions() const;
    void updateSynthOptions();
    void liveNoteOn (int note);
    void liveNoteOff (int note);
    void stopAll();
    void render (float* left, float* right, int numSamples);

    //==========================================================================
    // Plans, by id, for the audio thread. An id's plan is replaced whole and
    // the old one kept a while before it is freed: the audio thread reads it
    // only for as long as a trigger takes to copy it.
    static constexpr int maxPlans = 1 << 16;
    std::unique_ptr<std::atomic<const Plan*>[]> table;

    struct Slot
    {
        juce::String key;                  // partKey:index
        std::shared_ptr<const Plan> plan;
        double unwantedSinceMs = 0.0;      // 0 while in the song
    };

    struct Part
    {
        std::vector<voice::vocal::PartNote> notes;
        std::vector<int> ids;
        juce::uint32 generation = 0;
    };

    std::map<juce::String, int> idsByKey;
    std::map<int, Slot> slots;
    std::map<juce::String, Part> parts;
    int nextId = 0;
    juce::uint32 generation = 0;
    double plannedCenter = 0.0, plannedStrength = 1.0, plannedSpeed = 1.0;
    std::vector<std::pair<std::shared_ptr<const Plan>, double>> retired;

    //==========================================================================
    // The audio thread's.
    std::unique_ptr<voice::vocal::Synth> synth;
    double synthRate = 0.0;

    struct Applied
    {
        std::array<double, 21> values {};
        int vowelPreset = 0;
    };

    Applied applied;

    struct Live
    {
        bool active = false, held = false;
        int midi = 60;
        voice::vocal::Mora mora;
        double t = 0, off = 0;
    };

    Live live;
    voice::vocal::PlanState liveState;
    voice::vocal::EventList scratchEvents;
    int liveIndex = 0;

    // The live lyric's moras, swapped in under a lock the audio thread only tries.
    juce::SpinLock liveLock;
    std::array<voice::vocal::Mora, 64> liveMoras {};
    int numLiveMoras = 0;
    juce::String parsedLiveLyric;

    // The room: a small feedback delay network, in place of the prototype's
    // convolution with decaying noise (1.8s, a 0.33s time constant).
    struct Room
    {
        void prepare (double sampleRate);
        void process (const float* in, float* left, float* right, int n, float wet);

        static constexpr int numLines = 8;
        std::array<std::vector<float>, numLines> lines;
        std::array<int, numLines> positions {};
        std::array<float, numLines> feedback {}, damp {};
        std::array<float, numLines> state {};
    };

    Room room;
    std::array<float, 512> mono {};

    std::atomic<bool> voicedNow { false };
    std::atomic<float> sungHz { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalModelPlugin)
};

} // namespace carve::plugins
