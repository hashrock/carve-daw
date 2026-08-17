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

class Pattern
{
public:
    explicit Pattern (juce::ValueTree v) : state (std::move (v)) {}

    juce::String getId() const      { return state[ids::id]; }
    juce::String getName() const    { return state[ids::name]; }
    double getLengthBeats() const   { return state[ids::lengthBeats]; }

    void setName (const juce::String& n, juce::UndoManager* um)  { state.setProperty (ids::name, n, um); }
    void setLengthBeats (double beats, juce::UndoManager* um)    { state.setProperty (ids::lengthBeats, beats, um); }

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

    Pattern addPattern (const juce::String& name, double lengthBeats, juce::UndoManager* um);

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

    int getNumGenerators() const;
    Generator getGenerator (int index) const;
    std::vector<Generator> getGenerators() const;
    std::optional<Generator> findGenerator (const juce::String& generatorId) const;

    Generator addGenerator (const juce::String& name, const juce::String& type, juce::UndoManager* um);

    Playlist getPlaylist() const;

    juce::ValueTree state;
};

} // namespace orionish::model
