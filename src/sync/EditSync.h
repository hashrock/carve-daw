#pragma once

#include <optional>
#include <vector>

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
//
// rebuildInstruments throws away every generator track's instrument first, so
// each comes back from what the song says rather than from whatever the track
// had. The Edit outlives the song in the app -- loading a file rebinds a new
// EditSync to the same Edit, tracks and all -- and an instrument left over
// from the previous song would otherwise be taken for this one's and keep
// its old patch. EditSync passes this on its first sync only.
// Pattern mode: instead of the playlist, one pattern per generator -- the one
// its window last showed -- each placed at beat zero on its own track, so the
// user hears the patterns being worked on together and nothing else, Orion's
// Pattern/Song switch. The transport loops over the longest of them and the
// shorter ones repeat to fill that, so a one-bar hat pattern keeps going under
// a four-bar bass line the way it would on the playlist. A generator with no
// entry here plays nothing.
struct Audition
{
    struct Entry
    {
        juce::String generatorId, patternId;
        bool operator== (const Entry&) const = default;
    };

    // At most one entry per generator; the sync only ever looks a generator's
    // own entry up, so order is immaterial but kept in generator order by the
    // caller for stable value comparison.
    std::vector<Entry> entries;

    bool operator== (const Audition&) const = default;

    const Entry* findEntry (const juce::String& generatorId) const
    {
        for (const auto& entry : entries)
            if (entry.generatorId == generatorId)
                return &entry;

        return nullptr;
    }
};

// The loop Pattern mode runs: the longest auditioned pattern's length, which
// the others repeat to fill. Zero when no entry names a pattern with any
// length. One definition, so the clips the sync builds and the loop range
// the transport bar sets can never disagree about it.
double auditionLengthBeats (const model::Song& song, const Audition& audition);

void syncSongToEdit (const model::Song& song, te::Edit& edit, bool rebuildInstruments = false,
                     const Audition* audition = nullptr);

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

    // Switches between the playlist and the auditioned patterns (see Audition),
    // and resyncs on the spot: a mode switch should be heard on the next beat.
    // Compared by value, so handing over the same list again costs nothing --
    // the owner rebuilds it on every selection change without checking.
    void setAudition (std::optional<Audition> newAudition);
    const std::optional<Audition>& getAudition() const  { return audition; }

    // Fired after every full sync. The views that hold a pointer into the
    // Edit -- the generator window's instrument editor above all -- are
    // told about a model change before the sync that acts on it has run
    // (both arrive through the message loop), so a window opened on a
    // generator that was just added finds no track to look at yet. This is
    // where it gets to look again.
    std::function<void()> onSynced;

    // Copies each live external plugin's state (getStateInformation) back
    // into the model. Call before saving the song.
    void captureLivePluginState();

private:
    // True until the first sync has run: see syncSongToEdit's rebuildInstruments.
    bool firstSync = true;

    std::optional<Audition> audition;

    // A full resync tears down and rebuilds every MIDI clip, which glitches
    // playback. Mixer moves arrive continuously while a fader is dragged and
    // only ever touch track plugins, so they take a cheap path instead.
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        // The transport reads the loop range off the model itself, so nothing
        // here has to change for it.
        if (tree.hasType (model::ids::SONG) && model::Song::isLoopProperty (property))
            return;

        // A tempo or signature change moves every clip but changes none of
        // them, and a marker drag writes once per beat crossed.
        if (tree.hasType (model::ids::TEMPO) || tree.hasType (model::ids::TIMESIG))
        {
            applyTempoOnly();
            return;
        }

        // Dragging an automation point writes once per mouse move, and a
        // curve rewrite touches only plugin state -- no graph rebuild.
        if (tree.hasType (model::ids::PT) || tree.hasType (model::ids::AUTOCURVE))
        {
            applyAutomationOnly();
            return;
        }

        // Send knobs and return faders arrive continuously while dragged, and
        // only ever touch plugin parameters -- same cheap path as the mixer.
        if (tree.hasType (model::ids::SEND)
             || (tree.hasType (model::ids::RETURN) && property != model::ids::busNumber))
        {
            applySendsAndReturnsOnly();
            return;
        }

        if (tree.hasType (model::ids::GENERATOR) && model::Generator::isMixerProperty (property))
            applyMixerStateOnly();
        else
            triggerAsyncUpdate();
    }

    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&) override       { childListChanged (parent); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int) override { childListChanged (parent); }

    // Adding or removing an automation point is a child event on its lane;
    // everything else structural still takes the full resync.
    void childListChanged (const juce::ValueTree& parent)
    {
        if (parent.hasType (model::ids::AUTOCURVE) || parent.hasType (model::ids::AUTOMATION))
            applyAutomationOnly();
        else
            triggerAsyncUpdate();
    }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { triggerAsyncUpdate(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}

    void handleAsyncUpdate() override  { resyncNow(); }

    void applyMixerStateOnly();
    void applyTempoOnly();
    void applySendsAndReturnsOnly();
    void applyAutomationOnly();

    model::Song song;
    te::Edit& edit;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditSync)
};

} // namespace carve::sync
