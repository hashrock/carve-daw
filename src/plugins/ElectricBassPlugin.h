#pragma once

#include <array>
#include <atomic>
#include <initializer_list>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "BassDecoder.h"
#include "InstrumentFx.h"

namespace te = tracktion;

namespace carve::plugins
{

// A fingered electric bass, as an internal instrument, resynthesised from a
// 400KB analysis of a CC0 recording (DI, four dynamics, fourteen pitches;
// everything between is interpolated). BassDecoder.h is the engine.
//
// The knobs are a bassist's rather than a synth's: where the string is
// plucked (which notches the harmonics that have a node there), palm muting,
// how hard the fret buzzes, how much of the finger's attack there is, tone,
// body and sustain. Velocity sweeps the four recorded dynamics continuously.
//
// Played legato -- a note started while another is still held -- it can do
// what a bassist's left hand does instead of plucking again:
//
//   hammer  the new pitch from where the string's ring has got to, at the
//           same level, with a small fret tap (a pull-off, going down);
//   slide   the ringing note bent to the new one over Slide Time, landing
//           on the new note's own sound.
//
// Letting go of the newer key while the older is still held goes back to it,
// so trills work. All of this is the prototype's (web-bass app.js) logic,
// done per sample here rather than with Web Audio gain ramps.
class ElectricBassPlugin : public te::Plugin
{
public:
    explicit ElectricBassPlugin (te::PluginCreationInfo);
    ~ElectricBassPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Electric Bass"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Electric Bass"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Bass"; }
    juce::String getSelectableDescription() override  { return TRANS ("Electric Bass"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return false; }
    bool isSynth() override                           { return true; }
    bool producesAudioWhenNoAudioInput() override     { return true; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    enum class Technique { pluck, hammer, slide };
    static constexpr int numTechniques = 3;

    // A note marked in the piano roll with how to get to it
    // (model::Note::Technique) comes with a SysEx beside its note-on: this
    // id (the non-commercial manufacturer id, then "Cb"), the technique's
    // number -- 1 pluck, 2 hammer, 3 slide -- the note it is for, and how
    // long a slide takes in milliseconds as three 7-bit bytes, most
    // significant first (0: Slide Time). A marked hammer or slide moves from
    // the note before even if that note has been let go of, so long as it
    // is still ringing; a marked pluck plucks even over a held note.
    //
    // A slide out (model::Note::getSlideOut) comes on its own, part-way
    // through its note: the id, 4, the note, 1 up or 0 down, and how long in
    // milliseconds (three bytes). The note sounding is slid that way by Out
    // Frets and faded into nothing as it goes.
    static constexpr uint8_t sysexId[3] = { 0x7d, 0x43, 0x62 };
    static const char* getTechniqueName (Technique);

    Technique getTechnique() const;
    void setTechnique (Technique);
    bool isMono() const;
    void setMono (bool);

    // A slide steps through the frets it crosses; fretless, it glides.
    bool isFretless() const   { return fretless.load (std::memory_order_relaxed); }
    void setFretless (bool);

    // Right hand
    juce::CachedValue<float> pickValue, muteValue, attackValue, attackEqValue;
    // Tone
    juce::CachedValue<float> toneValue, bodyValue, buzzValue, sustainValue;
    // Left hand
    juce::CachedValue<float> slideTimeValue, tapValue, slideHoldValue, outFretsValue;
    // Setup
    juce::CachedValue<float> tuneValue, levelValue;

    // Amp: drive, then a three-band tone stack, then a level. Off until
    // switched on, so the bass as it comes is the recording.
    juce::CachedValue<float> ampDriveValue, ampBlendValue, ampBassValue, ampMidValue, ampMidFreqValue,
                             ampTrebleValue, ampLevelValue;

    te::AutomatableParameter::Ptr pick, mute, attack, attackEq,
                                  tone, body, buzz, sustain,
                                  slideTime, tap, slideHold, outFrets,
                                  tune, level,
                                  ampDrive, ampBlend, ampBass, ampMid, ampMidFreq, ampTreble, ampLevel;

    bool isAmpOn() const     { return ampOn.load (std::memory_order_relaxed); }
    void setAmpOn (bool);
    fx::BassAmp::Settings getAmpSettings() const;

    // For the editor's drawings.
    static const bass::Kit* getKit();
    bass::Options makeOptions() const;   // the knobs as the decoder takes them
    double getTuneSemitones() const      { return tune->getCurrentValue() / 100.0; }

    // The most recent note started, for the editor to draw (-1 for none yet).
    int getLastNote() const              { return lastNote.load (std::memory_order_relaxed); }
    float getLastVelocity() const        { return lastVelocity.load (std::memory_order_relaxed); }
    std::uint32_t getNoteCount() const   { return noteCount.load (std::memory_order_relaxed); }

private:
    struct PlayVoice;

    // 0 none, else 1 + Technique: what a trigger asked of a note.
    struct Mark { int technique = 0; double slideSeconds = 0.0; };   // technique 0: none

    void noteOn (int note, float velocity, Mark);
    void readTrigger (const juce::MidiMessage&, int samplePosition);
    Mark takeTrigger (int note, int samplePosition);
    void slideOff (const juce::MidiMessage&);
    void noteOff (int note);
    PlayVoice* pluck (int note, float velocity);
    PlayVoice* legato (PlayVoice& from, int note, Technique, double slideSeconds = 0.0);
    PlayVoice* allocate (std::initializer_list<const PlayVoice*> keep = {});
    PlayVoice* sounding (PlayVoice*);
    void crossfade (PlayVoice& from, PlayVoice& to, double seconds);
    void release (PlayVoice&, double seconds);
    void kill (PlayVoice&);
    void killAll();
    void render (float* out, int numSamples);

    juce::CachedValue<juce::String> techniqueValue;
    juce::CachedValue<bool> monoValue;
    juce::CachedValue<bool> ampOnValue, fretlessValue;
    std::atomic<bool> fretless { false };
    std::atomic<int> technique { 0 };
    std::atomic<bool> mono { true };
    std::atomic<bool> ampOn { false };
    fx::BassAmp amp;

    // Audio thread only.
    std::vector<std::unique_ptr<PlayVoice>> voices;
    bass::Spec spec;
    PlayVoice* lead = nullptr;
    std::array<int, 128> held {};      // held notes, oldest first

    // Triggers waiting for their note-on, by note: the technique (0 none)
    // and where it came, in samples from the start of this block (negative
    // for an earlier block). A SysEx and its note-on share a timestamp, and
    // which of the two comes first is not promised.
    struct PendingTrigger { int technique = 0; int position = 0; double slideSeconds = 0.0; };
    std::array<PendingTrigger, 128> triggers {};
    int numHeld = 0;
    std::uint64_t age = 0;
    std::vector<float> scratch;

    std::atomic<int> lastNote { -1 };
    std::atomic<float> lastVelocity { 0.0f };
    std::atomic<std::uint32_t> noteCount { 0 };

    struct StateWatcher : juce::ValueTree::Listener
    {
        explicit StateWatcher (ElectricBassPlugin& p) : owner (p) {}
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { owner.readSwitches(); }
        ElectricBassPlugin& owner;
    };

    void readSwitches();
    StateWatcher watcher { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ElectricBassPlugin)
};

} // namespace carve::plugins
