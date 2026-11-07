#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins
{

// The break slicer's instrument: one loop, cut into slices, slice i on note
// firstNote + i.
//
// It plays the way a slicer for breakcore has to:
//
//   * one slice at a time. A new note chokes the one sounding, with a short
//     fade, so a fast roll of the same slice is a stutter rather than a pile
//     of overlapping tails.
//   * as long as the note. Note-off fades the slice out, so a sixteenth on a
//     long slice plays a sixteenth of it -- which is what a chop is.
//   * up to the next cut and no further. A note longer than its slice stops
//     at the slice's end rather than running on into the next hit.
//
// A note can also play its slice backwards, slow it to a halt like a tape
// machine losing power, fade it in or out over the note, or scratch it
// forwards and back like a record
// (model::Note::PlayMode). A note-on cannot say any of that, so the sync
// puts a SysEx trigger beside such a note naming the mode and how long the
// note lasts (see PlayTrigger); a roll needs nothing here, since the sync
// writes it out as the notes it plays.
//
// What it plays is set from outside, like the SoundFont player: EditSync
// hands it the model's file, cuts and playback rate on every sync, and
// nothing of it is kept in its own tree.
//
// The loop is read once per file however many slicers play it, and read
// synchronously on the message thread, for the same reason the SoundFont
// player is: a render that started before it was in would open on silence.
class BreakPlugin : public te::Plugin
{
public:
    explicit BreakPlugin (te::PluginCreationInfo);
    ~BreakPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Break Slicer"); }
    static const char* xmlTypeName;

    static constexpr int firstNote = 36;   // Generator::breakFirstNote

    // The start of every SysEx this plugin reads: the non-commercial
    // manufacturer id, then "CB". After it come the mode (the number of a
    // model::Note::PlayMode), the note it is for, and the note's length in
    // milliseconds as three 7-bit bytes, most significant first. A buzz adds
    // the MIDI note it sounds and the rate it starts from, in tenths of a
    // hertz, three bytes the same way.
    static constexpr uint8_t sysexId[3] = { 0x7d, 0x43, 0x42 };

    enum class Mode { forward = 0, reverse = 1, tapeStop = 2, scratch = 3, fadeIn = 4, fadeOut = 5, buzz = 6 };

    juce::String getName() const override             { return "Break Slicer"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Break"; }
    juce::String getSelectableDescription() override  { return TRANS ("Break Slicer"); }

    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return false; }
    bool isSynth() override                           { return true; }
    bool producesAudioWhenNoAudioInput() override     { return true; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    // A read loop, shared between the slicers playing it.
    struct Audio
    {
        juce::File file;
        juce::Time stamp;
        juce::AudioBuffer<float> buffer;   // always two channels
        double sampleRate = 44100.0;

        double getLengthSeconds() const  { return buffer.getNumSamples() / sampleRate; }
    };

    // The loop and where it is cut (seconds, in order). Re-reads the file
    // only when it is a different one, or the same one rewritten since.
    //
    // repitchLoopBeats is how many beats the whole loop is when it is to be
    // repitched -- played faster or slower so that it lasts that many of the
    // song's beats at songBpm, pitch and all -- or 0 to play every slice as
    // recorded. Message thread only.
    void setSource (const juce::File&, const std::vector<double>& sliceStarts,
                    double repitchLoopBeats, double songBpm);

    // How fast the loop plays now: 1 as recorded.
    double getPlaybackRate() const;

    // What setSource last managed to load, or null. For the editor's waveform
    // and its zero-crossing snap.
    std::shared_ptr<const Audio> getAudio() const  { return audio; }

    // Why the last file did not load, or empty when it did.
    juce::String getLoadError() const  { return loadError; }

    // The slice sounding now and how far into the loop it is, for the
    // editor's playhead; -1 when nothing is. Written by the audio thread.
    int getPlayingSlice() const            { return playingSlice.load (std::memory_order_relaxed); }
    double getPlayingSeconds() const       { return playingSeconds.load (std::memory_order_relaxed); }

private:
    // What the audio thread plays from: swapped whole under the lock, so a
    // block sees one consistent loop and one set of cuts.
    struct Program
    {
        std::shared_ptr<const Audio> audio;
        std::vector<double> sliceStartFrames;   // in the file's own frames
        double rate = 1.0;
    };

    struct Voice
    {
        bool active = false;
        int note = -1;
        int slice = -1;
        Mode mode = Mode::forward;
        double position = 0.0;   // file frames
        double start = 0.0;      // file frames: this slice's cut
        double end = 0.0;        // file frames: the next cut
        double elapsed = 0.0;    // output samples since the note-on
        double duration = 0.0;   // output samples the note lasts, when a trigger said
        double buzzFrom = 0.0, buzzTo = 0.0;   // restarts a second, at the start and the end
        double cycle = 0.0;      // output samples into the current restart
        float gain = 1.0f;
        float envelope = 0.0f;
        float envelopeStep = 0.0f;   // per output sample; negative while fading out

        // Fading in or sustaining, rather than fading out. Zero is held: the
        // step drops to it when the attack is done, so "> 0" would be true
        // only for the first half millisecond of a note.
        bool isHeld() const  { return envelopeStep >= 0.0f; }
    };

    // A trigger read before its note-on: the two are written at the same
    // beat but can arrive in either order, a few samples apart, so whichever
    // comes second finds the first. Only good for a moment -- a trigger is
    // for a note at its own beat, never a later one.
    struct PendingTrigger
    {
        bool set = false;
        Mode mode = Mode::forward;
        double seconds = 0.0;
        double buzzFrom = 0.0, buzzTo = 0.0;
        double age = 0.0;   // output samples since it arrived
    };

    void handleMidi (const juce::MidiMessage&);
    void handleTrigger (const juce::MidiMessage&);
    void applyMode (Voice&, const PendingTrigger&);
    void startSlice (int note, float velocity);
    void releaseNote (int note);
    void releaseAll();
    void renderSegment (juce::AudioBuffer<float>&, int startSample, int numSamples);

    std::shared_ptr<const Audio> audio;
    std::unique_ptr<Program> program;
    juce::CriticalSection programLock;   // the audio thread only ever tries it

    juce::String loadError;

    // One sounding, the rest fading out from being choked. More than two
    // because a roll at 1/64 can choke faster than a fade finishes.
    std::array<Voice, 4> voices;
    std::array<PendingTrigger, 128> pending;
    bool anyPending = false;
    float attackStep = 1.0f, releaseStep = 1.0f;

    std::atomic<int> playingSlice { -1 };
    std::atomic<double> playingSeconds { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BreakPlugin)
};

} // namespace carve::plugins
