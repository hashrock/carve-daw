#pragma once

#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

struct tsf;

namespace carve::plugins
{

// An SF2 player, as an internal instrument. The synthesis is TinySoundFont's
// (one header, MIT; see NOTICE.md): sample playback per SF2 region, the
// volume and modulation envelopes, the per-voice low-pass, and the channel
// side of MIDI -- pitch bend, sustain, volume, pan, expression.
//
// What it plays is set from outside: EditSync hands it the model's file and
// preset on every sync (see Generator::getSoundFontFile), so nothing here is
// saved with the plugin and its own tree carries no settings.
//
// A font is read once per file however many generators play it. A General
// MIDI font is the usual reason to have one at all -- its piano on one
// generator, its bass on the next, its kit on a third -- and each of those
// re-reading a 100MB file would be three copies of it in memory. Every
// plugin playing the same file shares its samples and keeps only its own
// voices and channel state (TinySoundFont's tsf_copy).
//
// Loading is synchronous, on the message thread, for the same reason the
// renderer has to wait for a sampler's files before it starts: a render that
// began before the font was in would open on silence.
class SoundFontPlugin : public te::Plugin
{
public:
    explicit SoundFontPlugin (te::PluginCreationInfo);
    ~SoundFontPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("SoundFont"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "SoundFont"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "SF2"; }
    juce::String getSelectableDescription() override  { return TRANS ("SoundFont"); }

    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return false; }
    bool isSynth() override                           { return true; }
    bool producesAudioWhenNoAudioInput() override     { return true; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void reset() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    // One entry of the font's preset list.
    struct Preset
    {
        juce::String name;
        int bank = 0, program = 0;
    };

    // Points the player at a font and one of its presets. Re-reads the file
    // only when it is a different file (or the same one, rewritten since);
    // a preset change on the same font just retunes the channel. A preset the
    // font does not have falls back to its first. Message thread only.
    void setSource (const juce::File&, int bank, int program);

    // What setSource last managed: the loaded file (empty if none, or if it
    // could not be read), its presets in bank/program order, and the one
    // actually playing -- which is not the asked-for one after a fallback.
    juce::File getLoadedFile() const          { return loadedFile; }
    const std::vector<Preset>& getPresets() const;
    int getPlayingBank() const                { return playingBank; }
    int getPlayingProgram() const             { return playingProgram; }

    // Why the last file did not load, or empty when it did.
    juce::String getLoadError() const         { return loadError; }

    struct Font;   // a read file, shared between the plugins playing it

private:
    void applyOutputSettings (tsf&) const;
    void selectPreset (tsf&, int bank, int program);
    void handleMidi (tsf&, const juce::MidiMessage&);
    void renderSegment (tsf&, juce::AudioBuffer<float>&, int startSample, int numSamples);

    std::shared_ptr<Font> font;
    tsf* synth = nullptr;              // this plugin's voices over font's samples
    juce::CriticalSection synthLock;   // the audio thread only ever tries it

    juce::File loadedFile;
    juce::Time loadedStamp;
    juce::String loadError;
    int requestedBank = -1, requestedProgram = -1;
    int playingBank = 0, playingProgram = 0;

    // One render's worth of output, left then right: TinySoundFont's
    // "unweaved" layout, which wants the two halves contiguous.
    juce::HeapBlock<float> scratch;
    int scratchSamples = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundFontPlugin)
};

} // namespace carve::plugins
