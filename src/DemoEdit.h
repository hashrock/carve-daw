#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

// Builds the same demo song as the hand-rolled orionish engine, but as a
// tracktion Edit: one AudioTrack + 4OSC synth per "Generator", one MidiClip
// per playlist placement of a "Pattern".

namespace orionish
{

struct DemoNote
{
    double start, length;   // beats, relative to the pattern
    int pitch, velocity;
};

using Pattern = std::vector<DemoNote>;

inline void addPatternClip (te::AudioTrack& track, const juce::String& name,
                            double startBeat, double lengthBeats, const Pattern& pattern)
{
    const te::BeatRange beats (te::BeatPosition::fromBeats (startBeat),
                               te::BeatPosition::fromBeats (startBeat + lengthBeats));
    auto clip = track.insertMIDIClip (name, track.edit.tempoSequence.toTime (beats), nullptr);
    if (clip == nullptr)
        return;

    for (const auto& n : pattern)
        clip->getSequence().addNote (n.pitch,
                                     te::BeatPosition::fromBeats (n.start),
                                     te::BeatDuration::fromBeats (n.length),
                                     n.velocity, 0, nullptr);
}

inline te::AudioTrack* setupSynthTrack (te::Edit& edit, int index, const juce::String& name,
                                        float volumeDb, float pan)
{
    edit.ensureNumberOfAudioTracks (index + 1);
    auto track = te::getAudioTracks (edit)[index];
    track->setName (name);

    if (auto synth = dynamic_cast<te::FourOscPlugin*> (
            edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}).get()))
        track->pluginList.insertPlugin (*synth, 0, nullptr);

    if (auto volume = track->getVolumePlugin())
    {
        volume->setVolumeDb (volumeDb);
        volume->setPan (pan);
    }

    return track;
}

inline std::unique_ptr<te::Edit> buildDemoEdit (te::Engine& engine)
{
    auto edit = te::Edit::createSingleTrackEdit (engine);
    edit->tempoSequence.getTempo (0)->setBpm (120.0);

    const Pattern bassline = {
        { 0.0, 0.9,  45, 110 },
        { 1.0, 0.9,  45, 90 },
        { 2.0, 0.9,  52, 100 },
        { 3.0, 0.45, 53, 95 },
        { 3.5, 0.45, 55, 95 },
    };
    const Pattern chords = {
        { 0.0, 3.8, 57, 70 }, { 0.0, 3.8, 60, 70 }, { 0.0, 3.8, 64, 70 },   // Am
        { 4.0, 3.8, 53, 70 }, { 4.0, 3.8, 57, 70 }, { 4.0, 3.8, 60, 70 },   // F
    };
    const Pattern melody = {
        { 0.0, 0.45, 69, 100 }, { 0.5, 0.45, 72, 100 }, { 1.0, 0.9,  76, 100 },
        { 2.0, 0.45, 74, 100 }, { 2.5, 0.45, 72, 100 }, { 3.0, 0.9,  69, 100 },
        { 4.0, 0.45, 65, 100 }, { 4.5, 0.45, 69, 100 }, { 5.0, 0.9,  72, 100 },
        { 6.0, 1.8,  71, 100 },
    };

    if (auto bass = setupSynthTrack (*edit, 0, "Bass", -3.0f, 0.0f))
        for (double start : { 0.0, 4.0, 8.0, 12.0 })
            addPatternClip (*bass, "Bassline", start, 4.0, bassline);

    if (auto chordTrack = setupSynthTrack (*edit, 1, "Chords", -8.0f, -0.2f))
        for (double start : { 0.0, 8.0 })
            addPatternClip (*chordTrack, "Am F", start, 8.0, chords);

    if (auto lead = setupSynthTrack (*edit, 2, "Lead", -5.0f, 0.2f))
        addPatternClip (*lead, "Melody", 8.0, 8.0, melody);

    return edit;
}

} // namespace orionish
