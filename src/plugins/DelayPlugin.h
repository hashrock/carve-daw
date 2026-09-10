#pragma once

#include <vector>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins

{

// A delay with the controls a delay is normally expected to have.
//
// The engine ships one, and it has three knobs: time, feedback and mix. That
// is a delay you can put on a snare and nothing else -- no ping-pong, nothing
// in the feedback path to keep the repeats from piling up, no way to lock it
// to the tempo, and no movement. This one is the same idea with the rest of
// the front panel:
//
//   - **Sync**: free, or a note division of the song's tempo. Reads the tempo
//     at the block being rendered, so it follows a tempo change mid-song.
//   - **Ping-pong**, as an amount rather than a switch. At 0 the two channels
//     are independent delays; at 1 the input is summed into the left line and
//     each side's output feeds the other, which is the bouncing repeat. In
//     between it crossfeeds, which is the useful part nobody exposes.
//   - **Offset**: the right time as a percentage of the left. Uneven bounces
//     (dotted-against-straight) come from here.
//   - **Low cut / high cut in the feedback path**, not on the output: each
//     repeat goes round the loop again, so the tail darkens the way a tape or
//     bucket-brigade one does instead of turning to mud.
//   - **An LFO on the delay time**, in quadrature between the two channels, so
//     it widens as it wobbles. Small depths are chorus; large ones with a
//     short time are the tape flutter.
//   - **Width** on the wet signal, **mix**, and an output trim.
//
// Time changes glide rather than jump: the read position is slewed, so turning
// the knob bends the pitch of what is already in the line, which is what a
// delay is expected to do and what makes the sync divisions fun to switch
// between while it plays.
//
// Built-in like the rest of ours, so its settings are plain properties in the
// .carve and a song opens without a plugin scan (see EngineSetup.h).
class DelayPlugin : public te::Plugin
{
public:
    explicit DelayPlugin (te::PluginCreationInfo);
    ~DelayPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Delay"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Delay"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Delay"; }
    juce::String getSelectableDescription() override  { return TRANS ("Delay"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    // The note divisions Sync steps through, longest last. Index 0 is Free,
    // where the Time knob decides instead.
    static int getNumSyncDivisions();
    static const char* getSyncDivisionName (int index);
    static double getSyncDivisionBeats (int index);

    juce::CachedValue<float> syncValue, timeMsValue, offsetValue, feedbackValue, pingPongValue,
                             lowCutValue, highCutValue, modRateValue, modDepthValue,
                             widthValue, mixValue, outputDbValue;

    te::AutomatableParameter::Ptr sync, timeMs, offset, feedback, pingPong,
                                  lowCut, highCut, modRate, modDepth,
                                  width, mix, outputDb;

private:
    // A circular buffer read at a fractional position. The interpolation is
    // Catmull-Rom rather than linear: the LFO moves the read position every
    // sample, and linear interpolation there is a low pass whose corner moves
    // with it -- audible as a whistle riding the wobble.
    struct Line
    {
        void resize (int numSamples);
        void clear();
        void write (float x) noexcept;
        float read (double delaySamples) const noexcept;

        std::vector<float> buffer;
        int writePos = 0;
    };

    // One-pole pair, in the feedback loop. Not a steep filter on purpose:
    // this is the tone of the repeats, and a gentle slope is what lets the
    // tail fade in colour rather than step.
    struct OnePole
    {
        void setCoefficient (float c) noexcept  { a = c; }
        void reset() noexcept                   { z = 0.0f; }

        float lowPass (float x) noexcept   { z += a * (x - z); return z; }
        float highPass (float x) noexcept  { z += a * (x - z); return x - z; }

        float a = 1.0f, z = 0.0f;
    };

    static constexpr int maxChannels = 2;

    // Long enough for two beats at 30bpm (four seconds), which is as slow as
    // the longest division can get before the Time knob's own two-second
    // ceiling is the shorter limit anyway.
    static constexpr double maxDelaySeconds = 4.0;

    // Where the delay time is going, and where it is now: the second follows
    // the first a sample at a time, which is what bends the pitch of a repeat
    // when the time changes rather than clicking.
    double targetDelayL = 0.0, targetDelayR = 0.0;
    double currentDelayL = 0.0, currentDelayR = 0.0;
    double delaySlew = 0.0;

    double currentSampleRate = 44100.0;
    double lfoPhase = 0.0;

    te::tempo::Sequence::Position tempoPosition { te::createPosition (edit.tempoSequence) };

    Line lines[maxChannels];
    OnePole lowCutFilter[maxChannels], highCutFilter[maxChannels];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayPlugin)
};

} // namespace carve::plugins
