#pragma once

#include <optional>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "ModelIds.h"

// Typed wrappers around the ValueTree song document (Tracktion-style).
// Wrappers are cheap value types referencing shared tree state; all edits go
// through the tree so undo, listeners and serialisation come for free.

namespace carve::model
{

class Note
{
public:
    explicit Note (juce::ValueTree v) : state (std::move (v)) {}

    double getStart() const     { return state[ids::start]; }
    double getLength() const    { return state[ids::length]; }
    int getPitch() const        { return state[ids::pitch]; }
    int getVelocity() const     { return state[ids::velocity]; }

    void setStart (double beats, juce::UndoManager* um)   { state.setProperty (ids::start, beats, um); }
    void setLength (double beats, juce::UndoManager* um)  { state.setProperty (ids::length, beats, um); }
    void setPitch (int midiNote, juce::UndoManager* um)   { state.setProperty (ids::pitch, midiNote, um); }
    void setVelocity (int vel, juce::UndoManager* um)     { state.setProperty (ids::velocity, vel, um); }

    juce::ValueTree state;
};

// Every generator exposes a fixed grid of pattern slots, A1..D9, so the user
// can pick a slot and start drawing instead of creating a pattern first.
// A slot is addressed by (bank, index) but identified in the tree by its key
// ("A1"), so the grid can be resized later without rewriting old songs.
struct PatternSlot
{
    static constexpr int numBanks = 4;                          // A..D
    static constexpr int slotsPerBank = 9;                      // 1..9
    static constexpr int numSlots = numBanks * slotsPerBank;

    int bank = 0;
    int index = 0;

    bool isValid() const  { return bank >= 0 && bank < numBanks && index >= 0 && index < slotsPerBank; }
    int toFlatIndex() const  { return bank * slotsPerBank + index; }

    juce::String getKey() const
    {
        return juce::String::charToString ((juce::juce_wchar) ('A' + bank)) + juce::String (index + 1);
    }

    bool operator== (const PatternSlot& other) const  { return bank == other.bank && index == other.index; }
    bool operator!= (const PatternSlot& other) const  { return ! operator== (other); }

    static PatternSlot fromFlatIndex (int flat)  { return { flat / slotsPerBank, flat % slotsPerBank }; }

    // Returns nothing for an empty or unparseable key, which is what patterns
    // written before slots existed have.
    static std::optional<PatternSlot> fromKey (const juce::String& key);
};

class Pattern
{
public:
    explicit Pattern (juce::ValueTree v) : state (std::move (v)) {}

    // What a freshly materialised slot gets: one bar of 4/4.
    static constexpr double defaultLengthBeats = 4.0;

    juce::String getId() const      { return state[ids::id]; }
    juce::String getName() const    { return state[ids::name]; }
    double getLengthBeats() const   { return state[ids::lengthBeats]; }

    void setName (const juce::String& n, juce::UndoManager* um)  { state.setProperty (ids::name, n, um); }
    void setLengthBeats (double beats, juce::UndoManager* um)    { state.setProperty (ids::lengthBeats, beats, um); }

    // Which slot of its generator this pattern occupies, if any. The slot name
    // is only the pattern's default name, so a slot pattern can be renamed
    // freely and stays in its slot.
    std::optional<PatternSlot> getSlot() const  { return PatternSlot::fromKey (state[ids::slot].toString()); }
    void setSlot (const PatternSlot& s, juce::UndoManager* um)  { state.setProperty (ids::slot, s.getKey(), um); }

    bool isEmpty() const  { return getNumNotes() == 0; }

    // True while the pattern still carries the name its slot gave it, i.e. the
    // user never renamed it. Empty patterns that are still in this state were
    // only browsed past, so they can be dropped again.
    bool hasDefaultSlotName() const;

    int getNumNotes() const;
    Note getNote (int index) const;
    std::vector<Note> getNotes() const;

    Note addNote (double startBeats, double lengthBeats, int pitch, int velocity, juce::UndoManager* um);
    void removeNote (const Note& note, juce::UndoManager* um);

    juce::ValueTree state;
};


// One sample a sampler generator plays: a file, mapped across a key range and
// pitched from a root note. That is deliberately all of it -- one sample laid
// over the keyboard, or a handful with a root note each, covers the one-shot
// and drum-kit cases without a zone editor.
class SamplerSound
{
public:
    explicit SamplerSound (juce::ValueTree v) : state (std::move (v)) {}

    // A sound carrying nothing but a path plays its whole file across the
    // whole keyboard at unity gain, which is what a just-chosen sample should
    // do. Everything below is stored only once it differs from that.
    static constexpr int defaultRootNote = 60;      // middle C: the sample plays back untransposed there
    static constexpr int lowestNote = 0;
    static constexpr int highestNote = 127;

    juce::String getId() const    { return state[ids::id]; }
    juce::String getName() const  { return state[ids::name]; }

    void setName (const juce::String& n, juce::UndoManager* um)  { state.setProperty (ids::name, n, um); }

    // The sample, as an absolute path. A relative one is stored alongside it;
    // see Song::resolveSamplePaths for why, and which of the two wins.
    juce::File getFile() const;
    void setFile (const juce::File&, juce::UndoManager*);

    int getRootNote() const  { return state.getProperty (ids::rootNote, defaultRootNote); }
    int getMinNote() const   { return state.getProperty (ids::minNote, lowestNote); }
    int getMaxNote() const   { return state.getProperty (ids::maxNote, highestNote); }

    void setRootNote (int midiNote, juce::UndoManager* um)
    {
        state.setProperty (ids::rootNote, juce::jlimit (lowestNote, highestNote, midiNote), um);
    }

    void setKeyRange (int lowest, int highest, juce::UndoManager*);

    float getGainDb() const  { return state.getProperty (ids::gainDb, 0.0f); }
    float getPan() const     { return state.getProperty (ids::pan, 0.0f); }

    void setGainDb (float db, juce::UndoManager* um)  { state.setProperty (ids::gainDb, juce::jlimit (-48.0f, 48.0f, db), um); }
    void setPan (float p, juce::UndoManager* um)      { state.setProperty (ids::pan, juce::jlimit (-1.0f, 1.0f, p), um); }

    juce::ValueTree state;
};


// One insert effect on a generator's track. Either a tracktion internal
// plugin, named by its xmlTypeName, or an external VST3/AU described the same
// way the instrument is. The id is what lets EditSync match a model entry to
// the live plugin it already built, so parameter tweaks survive a resync.
class Effect
{
public:
    explicit Effect (juce::ValueTree v) : state (std::move (v)) {}

    static constexpr const char* externalType = "plugin";

    juce::String getId() const    { return state[ids::id]; }
    juce::String getType() const  { return state[ids::type]; }
    bool isExternal() const       { return getType() == externalType; }
    bool isEnabled() const        { return state.getProperty (ids::enabled, true); }

    void setEnabled (bool e, juce::UndoManager* um)  { state.setProperty (ids::enabled, e, um); }

    // For an external effect: which plugin, and its last captured state.
    void setPlugin (const juce::PluginDescription&, juce::UndoManager*);
    std::optional<juce::PluginDescription> getPluginDescription() const;
    void setPluginState (const juce::String& base64, juce::UndoManager* um)  { state.setProperty (ids::state, base64, um); }
    juce::String getPluginState() const  { return state[ids::state]; }

    // An internal tracktion plugin *is* its ValueTree, so its parameters are
    // kept by storing that tree here rather than as an opaque blob. Without
    // this every reload would hand the user a compressor back at its defaults.
    juce::ValueTree getInternalState() const;
    void setInternalState (const juce::ValueTree& pluginState, juce::UndoManager*);

    juce::ValueTree state;
};

class Generator
{
public:
    explicit Generator (juce::ValueTree v) : state (std::move (v)) {}

    juce::String getId() const      { return state[ids::id]; }
    juce::String getName() const    { return state[ids::name]; }
    juce::String getType() const    { return state[ids::type]; }

    // The one generator type the model itself has to reason about: a sampler
    // keeps its instrument's whole configuration in the tree, where the other
    // two only name a plugin.
    static constexpr const char* samplerType = "sampler";

    // A drum kit is the same SamplerPlugin with one sample per pad instead of
    // one stretched across the keyboard, so it differs only in how it is
    // presented and filled in -- everything downstream treats it as a sampler.
    static constexpr const char* drumKitType = "drum-sampler";

    bool isDrumKit() const  { return getType() == drumKitType; }
    bool isSampler() const  { return getType() == samplerType || isDrumKit(); }

    void setName (const juce::String& n, juce::UndoManager* um)  { state.setProperty (ids::name, n, um); }

    // Mixer state, applied to the generator's track by EditSync. Stored as
    // optional properties so older songs (and the demo) load with the defaults
    // a freshly created track used to get.
    static constexpr float defaultVolumeDb = -6.0f;

    float getVolumeDb() const  { return state.getProperty (ids::volumeDb, defaultVolumeDb); }
    float getPan() const       { return state.getProperty (ids::pan, 0.0f); }
    bool isMuted() const       { return state.getProperty (ids::mute, false); }
    bool isSoloed() const      { return state.getProperty (ids::solo, false); }

    void setVolumeDb (float db, juce::UndoManager* um)  { state.setProperty (ids::volumeDb, db, um); }
    void setPan (float p, juce::UndoManager* um)        { state.setProperty (ids::pan, p, um); }
    void setMuted (bool m, juce::UndoManager* um)       { state.setProperty (ids::mute, m, um); }
    void setSoloed (bool s, juce::UndoManager* um)      { state.setProperty (ids::solo, s, um); }

    // True for the properties above. Lets listeners treat mixer moves (which
    // arrive continuously while a fader is dragged) differently from
    // structural edits.
    static bool isMixerProperty (const juce::Identifier& property)
    {
        return property == ids::volumeDb || property == ids::pan
                || property == ids::mute || property == ids::solo;
    }

    // For type "plugin": which external plugin this generator hosts, and its
    // saved state (base64 of getStateInformation). The state is captured from
    // the live instance on save and restored on load.
    void setPlugin (const juce::PluginDescription&, juce::UndoManager*);
    std::optional<juce::PluginDescription> getPluginDescription() const;
    void setPluginState (const juce::String& base64, juce::UndoManager*);
    juce::String getPluginState() const;

    // For type "sampler": the samples it plays, in the order the sampler gets
    // them. Empty for every other type, and the SOUNDS node stays out of the
    // tree entirely until a sample is assigned, so nothing else changes shape.
    std::vector<SamplerSound> getSounds() const;

    SamplerSound addSound (const juce::File&, juce::UndoManager*);
    void removeSound (const SamplerSound&, juce::UndoManager*);

    // Points the generator at one sample across the whole keyboard, dropping
    // whatever it had. This is what the panel's file chooser and file drops
    // do: the tree can hold a multi-sample kit, but nothing edits one yet.
    SamplerSound setSingleSound (const juce::File&, juce::UndoManager*);

    // Insert effects, in signal order, sitting between the instrument and the
    // track fader.
    std::vector<Effect> getEffects() const;
    std::optional<Effect> findEffect (const juce::String& effectId) const;

    // `type` is a tracktion xmlTypeName, or Effect::externalType with a
    // description for a VST3/AU.
    Effect addEffect (const juce::String& type, const juce::PluginDescription*, juce::UndoManager*);
    void removeEffect (const Effect&, juce::UndoManager*);
    void moveEffect (const Effect&, int newIndex, juce::UndoManager*);

    int getNumPatterns() const;
    Pattern getPattern (int index) const;
    std::vector<Pattern> getPatterns() const;
    std::optional<Pattern> findPattern (const juce::String& patternId) const;

    // Patterns that don't sit in a slot: everything in a song written before
    // slots existed, plus anything the user adds beyond the grid. The slot
    // picker lists them separately so they stay reachable.
    std::vector<Pattern> getUnslottedPatterns() const;

    std::optional<Pattern> findPatternInSlot (const PatternSlot& slot) const;

    // Materialises the slot the first time it is used. Empty slots stay out of
    // the tree deliberately: writing all 36 into every generator would bloat
    // every .carve and make every EditSync resync walk dead nodes.
    Pattern getOrCreatePatternInSlot (const PatternSlot& slot, juce::UndoManager* um);

    Pattern addPattern (const juce::String& name, double lengthBeats, juce::UndoManager* um);
    Pattern addPattern (const PatternSlot& slot, const juce::String& name,
                        double lengthBeats, juce::UndoManager* um);
    void removePattern (const Pattern& pattern, juce::UndoManager* um);

    juce::ValueTree state;
};

class PlaylistClip
{
public:
    explicit PlaylistClip (juce::ValueTree v) : state (std::move (v)) {}

    juce::String getGeneratorId() const  { return state[ids::generatorId]; }
    juce::String getPatternId() const    { return state[ids::patternId]; }
    double getStart() const              { return state[ids::start]; }

    void setStart (double beats, juce::UndoManager* um)  { state.setProperty (ids::start, beats, um); }

    // Which of the generator's patterns this placement plays. Only patterns
    // belonging to the clip's own generator make sense here.
    void setPatternId (const juce::String& id, juce::UndoManager* um)  { state.setProperty (ids::patternId, id, um); }

    // How long this placement plays for, independent of its pattern: shorter
    // cuts the pattern off, longer repeats it. Absent means "as long as the
    // pattern", which is what every clip was before this existed — so older
    // songs keep playing exactly as they did.
    bool hasOwnLength() const  { return state.hasProperty (ids::length); }

    double getLength (double patternLengthBeats) const
    {
        const double length = state.getProperty (ids::length, patternLengthBeats);
        return length > 0.0 ? length : patternLengthBeats;
    }

    void setLength (double beats, juce::UndoManager* um)
    {
        state.setProperty (ids::length, std::max (0.0625, beats), um);
    }

    void clearLength (juce::UndoManager* um)  { state.removeProperty (ids::length, um); }

    // Semitones added to every note of the pattern, for this placement only.
    int getTranspose() const  { return state.getProperty (ids::transpose, 0); }

    void setTranspose (int semitones, juce::UndoManager* um)
    {
        state.setProperty (ids::transpose, juce::jlimit (-48, 48, semitones), um);
    }

    juce::ValueTree state;
};

class Playlist
{
public:
    explicit Playlist (juce::ValueTree v) : state (std::move (v)) {}

    int getNumClips() const;
    PlaylistClip getClip (int index) const;
    std::vector<PlaylistClip> getClips() const;

    PlaylistClip addClip (const Generator& generator, const Pattern& pattern,
                          double startBeats, juce::UndoManager* um);
    void removeClip (const PlaylistClip& clip, juce::UndoManager* um);

    juce::ValueTree state;
};

class Song
{
public:
    explicit Song (juce::ValueTree v) : state (std::move (v)) {}

    static Song create (const juce::String& name);
    static std::optional<Song> fromXml (const juce::String& xml);
    static std::optional<Song> loadFromFile (const juce::File& file);

    juce::String toXmlString() const;
    bool saveToFile (const juce::File& file) const;

    // Sampler paths, which are the only thing in the model that means anything
    // outside the file. Each sound stores both an absolute path and one
    // relative to the .carve: resolving prefers the relative one when it lands
    // on a file that exists, because that is the case where the absolute one
    // is wrong (the project was copied elsewhere, and the old path may still
    // exist on this machine pointing at a different sample). Refreshing
    // rewrites the relative path from the absolute one against wherever the
    // song is being saved. loadFromFile and saveToFile call these, and both
    // do nothing at all to a song without sampler generators.
    void resolveSamplePaths (const juce::File& songFile) const;
    void refreshSamplePaths (const juce::File& songFile) const;

    juce::String getName() const  { return state[ids::name]; }
    double getTempo() const       { return state[ids::tempo]; }

    void setName (const juce::String& n, juce::UndoManager* um)  { state.setProperty (ids::name, n, um); }
    void setTempo (double bpm, juce::UndoManager* um)            { state.setProperty (ids::tempo, bpm, um); }

    // Playback loop range in beats, set from the playlist ruler. An empty
    // range means "loop the whole song", which is what playback did before
    // the range existed, so older songs keep their behaviour.
    double getLoopStart() const  { return state.getProperty (ids::loopStart, 0.0); }
    double getLoopEnd() const    { return state.getProperty (ids::loopEnd, 0.0); }
    bool hasLoopRange() const    { return getLoopEnd() > getLoopStart(); }

    void setLoopRange (double startBeats, double endBeats, juce::UndoManager*);
    void clearLoopRange (juce::UndoManager*);

    // The loop range is read straight off the model by the transport, so
    // nothing has to be rebuilt when it changes. Dragging one on the ruler
    // writes it once per bar crossed, so the difference matters.
    static bool isLoopProperty (const juce::Identifier& property)
    {
        return property == ids::loopStart || property == ids::loopEnd;
    }

    int getNumGenerators() const;
    Generator getGenerator (int index) const;
    std::vector<Generator> getGenerators() const;
    std::optional<Generator> findGenerator (const juce::String& generatorId) const;

    Generator addGenerator (const juce::String& name, const juce::String& type, juce::UndoManager* um);

    Playlist getPlaylist() const;

    // Whether anything on the playlist plays this pattern. Used to decide if a
    // slot the user only browsed past can be dropped again.
    bool isPatternUsedInPlaylist (const Pattern& pattern) const;

    juce::ValueTree state;
};

} // namespace carve::model
