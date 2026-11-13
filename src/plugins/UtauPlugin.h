#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"
#include "voice/Voicebank.h"
#include "voice/VoiceSynth.h"

namespace te = tracktion;

namespace carve::plugins
{

// A UTAU singer, as an internal instrument.
//
// UTAU sings a note from where its consonant starts -- before the beat, by the
// sample's preutterance -- and shapes it by the notes either side: which
// recording it takes (the connected "a か" after a note ending in a, "- か" at
// the start of a phrase), where it fades in over the previous note's tail and
// out under the next one's head, the glide into its pitch. None of that fits
// in a MIDI note-on, and a WORLD synthesis takes far longer than a block.
//
// So a note is prepared ahead of time, as UTAU itself does: EditSync hands
// prepare() each note of a vocal part with its neighbours, gets back an id and
// the preutterance, and puts a trigger -- a SysEx carrying the id -- into the
// clip that much ahead of the note. The note is synthesised on a worker
// thread meanwhile, and the trigger plays the result. The same note in the
// same context is the same id, so an edit re-synthesises only the notes it
// touched and their neighbours.
//
// Live notes -- a keyboard, the piano roll's previews -- arrive as plain MIDI
// with no lyric and no time to synthesise, so they play the bank's "あ"
// recording, transposed. A guide, not the song.
class UtauPlugin : public te::Plugin,
                   private juce::Timer
{
public:
    explicit UtauPlugin (te::PluginCreationInfo);
    ~UtauPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("UTAU Singer"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "UTAU Singer"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "UTAU"; }
    juce::String getSelectableDescription() override  { return TRANS ("UTAU Singer"); }

    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return false; }
    bool isSynth() override                           { return true; }
    bool producesAudioWhenNoAudioInput() override     { return true; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    //==========================================================================
    // Message thread.

    // Loads the voicebank in this folder, if it is not the one already
    // loaded. Every prepared note is dropped when the bank changes.
    void setVoicebank (const juce::File& folder);

    std::shared_ptr<const voice::Voicebank> getVoicebank() const  { return bank; }

    // How the voice is coloured. Every note sung from here on is synthesised
    // with it; a change is a change to every note's key, so the next prepare
    // pass re-synthesises the lot.
    void setVoiceStyle (const model::Generator::VoiceStyle& newStyle)  { style = newStyle; }
    juce::String getLoadError() const                            { return loadError; }

    // A note of a vocal part, with what it needs to know about its
    // neighbours. "Straight before/after" means touching or overlapping: a
    // gap between two notes is a breath, and the next one starts a phrase.
    struct NoteContext
    {
        juce::String lyric;                  // empty: Note::getDefaultLyric()
        int pitch = 60;
        int velocity = 100;
        double lengthSec = 0.5;

        bool hasPrevious = false;            // a note straight before
        juce::String previousLyric;
        int previousPitch = 60;
        double previousLengthSec = 0.0;

        bool hasNext = false;                // a note straight after
        juce::String nextLyric;
        int nextPitch = 60;
        double nextStartSec = 0.0;           // from this note's start
    };

    // What to put in the clip: a trigger for snippet `id`, leadSec before
    // the note. id is negative when the bank has nothing to sing the lyric
    // with (or no bank is loaded) -- the note is silent.
    struct Prepared
    {
        int id = -1;
        double leadSec = 0.0;
    };

    // One pass of EditSync over this generator's parts: begin, prepare each
    // note, end. A note prepared in the last pass and not this one is no
    // longer in the song, and its audio is let go a little later.
    void beginPreparing();
    Prepared prepare (const NoteContext&);
    void endPreparing();

    // Notes still being synthesised. A render of the song has to wait for
    // these, or it would have holes where they go.
    int getNumPending() const   { return renderState->pending.load(); }
    int getNumFailed() const    { return renderState->failed.load(); }
    juce::String getLastRenderError() const;

    // Blocks until nothing is pending, or the timeout passes. Any thread.
    bool waitForRenders (int timeoutMs) const;

    // The trigger SysEx: plays snippet `id`, starting skipSec into it -- for
    // a note whose lead-in would start before its clip does.
    static juce::MidiMessage createTrigger (int id, double skipSec);
    static bool parseTrigger (const juce::MidiMessage&, int& id, double& skipSec);

    // What a trigger plays: one note, synthesised. The audio is written once
    // by its worker and read-only after `ready`.
    struct Snippet
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
        std::atomic<bool> ready { false };
        std::atomic<int> playing { 0 };     // voices reading it right now

        // Whether the song still has this note. A synthesis that has not
        // started by the time it is no longer wanted -- a knob moved, a
        // phrase rewritten -- is skipped, and started again should the note
        // come back.
        std::atomic<bool> wanted { true };
        std::atomic<bool> skipped { false };
    };

    // Shared with the worker jobs, which can outlive the plugin.
    struct RenderState
    {
        std::atomic<int> pending { 0 }, failed { 0 };
        juce::CriticalSection errorLock;
        juce::String lastError;
    };

private:
    void timerCallback() override;
    void submit (std::shared_ptr<Snippet>, const voice::NoteRender&);
    void dropAllSnippets();
    void retire (int id);

    struct Voice
    {
        Snippet* snippet = nullptr;          // a sung note, or
        const juce::AudioBuffer<float>* raw = nullptr;   // the live-note recording
        double position = 0.0, step = 1.0;
        int note = -1;                       // live notes only, for their note-off
        float gain = 1.0f;
        int releaseLeft = -1;                // counting down a fade-out; -1 while held
        int releaseLength = 1;
        bool active = false;
    };

    void startSnippet (int id, double skipSec);
    void startLiveNote (int note, float velocity);
    void releaseLiveNote (int note);
    void stopAll (int fadeSamples);
    void renderVoices (juce::AudioBuffer<float>&, int start, int numSamples);
    void finish (Voice&);

    std::shared_ptr<const voice::Voicebank> bank;
    juce::String loadError;
    model::Generator::VoiceStyle style;

    // The live-note recording, swapped in and out under the lock the audio
    // thread only tries.
    std::shared_ptr<voice::RawSample> liveSample;
    juce::CriticalSection liveLock;

    // id -> snippet, read by the audio thread. Ids are handed out in order and
    // wrap; a slot is only reused long after what was in it went quiet.
    static constexpr int maxSnippets = 1 << 16;
    std::unique_ptr<std::atomic<Snippet*>[]> table;

    struct Slot
    {
        juce::String key;
        std::shared_ptr<Snippet> snippet;
        juce::uint32 generation = 0;
        double unwantedSinceMs = 0.0;        // 0 while still in the song
        voice::NoteRender request;           // to synthesise it again after a skip
    };

    std::map<juce::String, int> idsByKey;
    std::map<int, Slot> slots;
    int nextId = 0;
    juce::uint32 generation = 0;

    // Let go of but perhaps still being read: freed once quiet for a while.
    struct Retired
    {
        std::shared_ptr<Snippet> snippet;
        double sinceMs = 0.0;
    };
    std::vector<Retired> retired;

    // Live-note recordings replaced by a new bank: a held key may still be
    // reading one, and a low note reads it slowly.
    std::vector<std::pair<std::shared_ptr<voice::RawSample>, double>> retiredLiveSamples;

    std::shared_ptr<RenderState> renderState = std::make_shared<RenderState>();

    std::array<Voice, 32> voices;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UtauPlugin)
};

} // namespace carve::plugins
