#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace orionish::sync
{

// One-shot, idempotent sync of the whole song model into a tracktion Edit:
//  - tempo -> TempoSequence
//  - each Generator -> AudioTrack + FourOscPlugin instrument
//  - each Playlist placement -> MidiClip filled from the referenced Pattern
// Patterns stay first-class in the model; clips are disposable expansions,
// so editing a pattern updates every placement on the next sync.
void syncSongToEdit (const model::Song& song, te::Edit& edit);

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

} // namespace orionish::sync
