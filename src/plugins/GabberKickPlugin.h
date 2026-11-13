#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "GabberKickDsp.h"

namespace te = tracktion;

namespace carve::plugins
{

// A gabber/hardcore kick designer, as an internal instrument: the patch is
// rendered offline (see GabberKickDsp.h) and the finished kick is what a note
// plays.
//
// Note 36 plays the kick at the pitch it was designed; other notes repitch it,
// a sampler's way, because tuned kicks are half of what this music does with
// them. One kick at a time: a new hit cuts the last with a few milliseconds of
// fade, since two of these overlapping is two limited-to-the-ceiling signals
// adding up.
//
// The patch lives in the plugin's tree like any internal plugin's settings, so
// songs and presets carry it without knowing what it is. The rendered kick is
// derived from it, never stored: a change on the message thread wakes a
// renderer thread, and the audio thread picks up the result at its next block.
class GabberKickPlugin : public te::Plugin
{
public:
    explicit GabberKickPlugin (te::PluginCreationInfo);
    ~GabberKickPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Kick Designer"); }
    static const char* xmlTypeName;

    static constexpr int rootNote = 36;

    juce::String getName() const override             { return "Kick Designer"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Kick"; }
    juce::String getSelectableDescription() override  { return TRANS ("Kick Designer"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return false; }
    bool isSynth() override                           { return true; }
    bool producesAudioWhenNoAudioInput() override     { return true; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    // The editor's side. The patch is read and written whole: every edit is
    // get, change one field, set.
    gabber::Patch getPatch() const;
    void setPatch (const gabber::Patch&);

    // The factory preset the patch currently is, or -1 once it has been edited.
    int getMatchingFactoryPreset() const;

    // Moves whenever the patch actually changes, however it was changed.
    std::uint64_t getPatchRevision() const;

    struct RenderedKick
    {
        std::vector<float> samples;
        double sampleRate = 44100.0, peak = 0.0;
        bool limited = false;
        std::uint64_t revision = 0;
    };

    // Null until the first render is done.
    std::shared_ptr<const RenderedKick> getRenderedKick() const;

    // True while what can be heard is behind the patch.
    bool isRenderPending() const;

private:
    struct StateWatcher : juce::ValueTree::Listener
    {
        explicit StateWatcher (GabberKickPlugin& p) : owner (p) {}
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { owner.stateChanged(); }
        GabberKickPlugin& owner;
    };

    struct RenderThread : juce::Thread
    {
        explicit RenderThread (GabberKickPlugin& p) : juce::Thread ("Carve Gabber Kick"), owner (p) {}
        void run() override;
        GabberKickPlugin& owner;
    };

    struct Voice
    {
        std::shared_ptr<const RenderedKick> kick;
        double position = 0.0, increment = 1.0, gain = 1.0;
        double fade = 1.0, fadeStep = 0.0;    // fadeStep 0: not fading out
    };

    gabber::Patch readPatchFromState() const;
    void stateChanged();
    void renderIfStale();
    void trigger (int note, float velocity);
    void renderVoices (float* out, int numSamples);

    // Under patchLock: what should be rendered, and at what rate.
    juce::CriticalSection patchLock;
    gabber::Patch latestPatch;
    std::uint64_t latestRevision = 1;
    double renderRate = 44100.0;

    juce::CriticalSection renderLock;     // one render at a time: the thread, or initialise()

    // The audio thread takes `published` with a try-lock, so publishing never
    // blocks it. `retained` keeps a reference to every kick a voice might hold,
    // so that a voice letting go is never what frees one: the last reference
    // is always dropped here, off the audio thread.
    mutable juce::SpinLock publishLock;
    std::shared_ptr<const RenderedKick> published;
    std::vector<std::shared_ptr<const RenderedKick>> retained;

    // Audio thread only.
    std::shared_ptr<const RenderedKick> live;
    std::array<Voice, 2> voices;
    double sampleRate = 44100.0;

    bool writingPatch = false;
    StateWatcher watcher { *this };
    RenderThread renderThread { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GabberKickPlugin)
};

} // namespace carve::plugins
