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

namespace orionish::model
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

class Generator
{
public:
    explicit Generator (juce::ValueTree v) : state (std::move (v)) {}

    juce::String getId() const      { return state[ids::id]; }
    juce::String getName() const    { return state[ids::name]; }
    juce::String getType() const    { return state[ids::type]; }

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
    // every .orion and make every EditSync resync walk dead nodes.
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

} // namespace orionish::model
