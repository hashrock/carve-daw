#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::sync
{

// One-shot, idempotent sync of the whole song model into a tracktion Edit:
//  - tempo -> TempoSequence
//  - each Generator -> AudioTrack + whatever instrument its type calls for
//  - each Playlist placement -> MidiClip filled from the referenced Pattern
// Patterns stay first-class in the model; clips are disposable expansions,
// so editing a pattern updates every placement on the next sync.
//
// An "audio" generator is the exception at both ends: its track hosts no
// instrument, and its placements become WaveAudioClips pointing straight at
// the files. Those are matched by id and updated in place rather than rebuilt,
// because re-inserting one re-reads the file and cuts what it is playing.
void syncSongToEdit (const model::Song& song, te::Edit& edit);

// Samplers pick their sounds up out of their own state from a message-loop
// callback, so anything that renders straight after syncing would get silence.
// A GUI's loop delivers that callback on its own a moment later; a headless
// caller has to ask for it, and must do so before rendering.
void flushSamplerLoads (te::Edit& edit);

// Keeps an Edit continuously in sync with a song model: any change to the
// song tree triggers a debounced (coalesced per message-loop tick) full
// resync. Message thread only.
class EditSync : private juce::ValueTree::Listener,
                 private juce::AsyncUpdater
{
public:
    EditSync (model::Song songToWatch, te::Edit& targetEdit);
    ~EditSync() override;

    void resyncNow();

    // Copies each live external plugin's state (getStateInformation) back
    // into the model. Call before saving the song.
    void captureLivePluginState();

private:
    // A full resync tears down and rebuilds every MIDI clip, which glitches
    // playback. Mixer moves arrive continuously while a fader is dragged and
    // only ever touch track plugins, so they take a cheap path instead.
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        // The transport reads the loop range off the model itself, so nothing
        // here has to change for it.
        if (tree.hasType (model::ids::SONG) && model::Song::isLoopProperty (property))
            return;

        if (tree.hasType (model::ids::GENERATOR) && model::Generator::isMixerProperty (property))
            applyMixerStateOnly();
        else
            triggerAsyncUpdate();
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { triggerAsyncUpdate(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}

    void handleAsyncUpdate() override  { resyncNow(); }

    void applyMixerStateOnly();

    model::Song song;
    te::Edit& edit;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditSync)
};

} // namespace carve::sync
