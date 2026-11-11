#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "Loudness.h"

namespace te = tracktion;

namespace carve::plugins
{

// What a soloed reference track plays: the last plugin but one on the master,
// after the master's effects and the mastering chain, so the reference
// replaces the mix entirely. Nothing soloed (A), it passes the mix through
// and measures its loudness; a reference soloed (B), it plays that track's
// clips at the song's position instead, so the two can be flipped between
// mid-phrase. Matched, the reference is played at the mix's loudness --
// integrated LUFS, BS.1770 -- since the louder of two things always sounds
// better and the comparison is meant to be about everything else.
//
// It is not part of the song: the app puts it on the master, keeps it there
// and tells it which clips to play. The reference track's own Edit track is
// muted, so a reference is never in a render -- an export is the mix,
// whatever is soloed.
class ReferencePlayerPlugin : public te::Plugin
{
public:
    explicit ReferencePlayerPlugin (te::PluginCreationInfo);
    ~ReferencePlayerPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Reference Player"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Reference Player"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Ref"; }
    juce::String getSelectableDescription() override  { return TRANS ("Reference Player"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override;

    //==============================================================================
    // One clip of a reference track, in seconds: where it sits in the song,
    // how long, and where in its file it starts.
    struct Clip
    {
        juce::File file;
        double start = 0.0, length = 0.0, fileOffset = 0.0;

        bool operator== (const Clip& o) const
        {
            return file == o.file && std::abs (start - o.start) < 1.0e-6 && std::abs (length - o.length) < 1.0e-6
                    && std::abs (fileOffset - o.fileOffset) < 1.0e-6;
        }
    };

    // What to play. Files are read on a thread of their own and kept while
    // any clip uses them; until the new set is ready the old one plays.
    // timeShift moves the song's clock, for Pattern mode: that loops from
    // zero, and plays the reference from its first clip.
    void setClips (std::vector<Clip>, float trimDb, bool levelMatched, double timeShiftSeconds);

    // B (true) or A.
    void setListening (bool shouldPlayReference);
    bool isListening() const  { return listening.load (std::memory_order_relaxed); }

    enum class Status { none, loading, ready, failed };
    Status getStatus() const  { return status.load (std::memory_order_relaxed); }

    // Integrated loudness of the reference as placed, and of the mix as
    // played since playback last started (or before, if nothing has been
    // played since). -inf for not known.
    double getReferenceLufs() const  { return referenceLufs.load (std::memory_order_relaxed); }
    double getMixLufs() const        { return mixLufs.load (std::memory_order_relaxed); }

    // The gain the reference is played at now, in dB.
    double getReferenceGainDb() const  { return appliedGainDb.load (std::memory_order_relaxed); }

    // The reference's long-term average spectrum, for the analyzer to lay
    // over the mix's: power per FFT bin, in dB, as recorded (add
    // getReferenceGainDb for as played). Null until one is loaded.
    struct Spectrum
    {
        int fftOrder = 13;
        double sampleRate = 48000.0;
        std::vector<float> db;   // fftSize / 2 + 1 bins
    };

    std::shared_ptr<const Spectrum> getReferenceSpectrum() const;

private:
    struct Audio
    {
        juce::File file;
        double sampleRate = 0.0;
        juce::AudioBuffer<float> samples;   // at sampleRate, two channels
    };

    // What the audio thread plays: the clips in samples, each with its file.
    struct Program
    {
        struct Placed
        {
            juce::int64 start = 0, length = 0, fileOffset = 0;
            std::shared_ptr<const Audio> audio;
        };

        std::vector<Clip> clips;   // as asked for, to tell whether it is stale
        double sampleRate = 0.0;
        std::vector<Placed> placed;   // by start
        double lufs = -INFINITY;
        std::shared_ptr<const Spectrum> spectrum;
    };

    struct Loader : juce::Thread
    {
        explicit Loader (ReferencePlayerPlugin& p) : juce::Thread ("Carve Reference Loader"), owner (p) {}
        void run() override;
        ReferencePlayerPlugin& owner;
    };

    void buildIfStale();
    std::shared_ptr<const Audio> readFile (const juce::File&, double rate);
    std::shared_ptr<const Program> getProgram() const;

    // Message-thread side, under requestLock.
    juce::CriticalSection requestLock;
    std::vector<Clip> wantedClips;
    double wantedRate = 0.0;

    std::atomic<float> trimDb { 0.0f };
    std::atomic<double> timeShift { 0.0 };
    std::atomic<bool> matched { true }, listening { false };
    std::atomic<Status> status { Status::none };
    std::atomic<double> referenceLufs { -INFINITY }, mixLufs { -INFINITY }, appliedGainDb { 0.0 };

    // Loader thread only: every file read, until no program uses it.
    std::map<juce::String, std::shared_ptr<const Audio>> files;

    // Published to the audio thread the way the Gabber Kick's render is: a
    // try-lock to take, and every one ever handed out kept here until no one
    // else holds it, so the audio thread never frees one.
    mutable juce::SpinLock publishLock;
    std::shared_ptr<const Program> published;
    std::vector<std::shared_ptr<const Program>> retained;

    // Audio thread only.
    std::shared_ptr<const Program> live;
    LoudnessMeter mixMeter;
    double lastMixLufs = -INFINITY;
    bool wasPlaying = false;
    float mixAmount = 1.0f;            // 1: the mix, 0: the reference
    double gainNow = 1.0;

    Loader loader { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReferencePlayerPlugin)
};

} // namespace carve::plugins
