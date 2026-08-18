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

    // What a freshly materialised slot gets when the caller has no song to ask:
    // one bar of 4/4.
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

    // Drops every note, keeping the pattern's id, name, slot and length -- so
    // everything already pointing at it (a playlist clip, the piano roll) still
    // is. What a MIDI import needs before it writes the file's notes in.
    void clearNotes (juce::UndoManager* um);

    // Replaces this pattern's notes with copies of another's. The source may
    // belong to any generator: a note names a pitch and nothing about the
    // instrument that sounds it, so a pattern crosses generators unchanged.
    void copyNotesFrom (const Pattern& source, juce::UndoManager* um);

    juce::ValueTree state;
};


// How a node that points at a file on disk stores it: an absolute path in
// `file`, plus the same file relative to the .carve in `relPath`. Resolving
// prefers the relative one when it lands on a file that exists, because that
// is exactly the case where the absolute one is wrong (the project was copied
// elsewhere, and the old path may still exist on this machine pointing at
// something else). Both sampler sounds and playlist audio clips use this, so
// a song moved with its media plays the same anywhere.
struct FileRef
{
    static juce::File getFile (const juce::ValueTree&);
    static void setFile (juce::ValueTree, const juce::File&, juce::UndoManager*);

    // Called by Song::loadFromFile / saveToFile once the song's own location
    // is known, which is the only thing a relative path is relative to.
    static void resolve (juce::ValueTree, const juce::File& songDirectory);
    static void refresh (juce::ValueTree, const juce::File& songDirectory);
};

// How long an audio file is, in seconds, or 0 for anything that can't be
// read. The playlist needs this to give a dropped file a length, and that is
// the only thing in the model that has to look at audio data rather than at
// the tree.
double readAudioFileLengthSeconds (const juce::File&);


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
    // see FileRef for why, and which of the two wins.
    juce::File getFile() const                                   { return FileRef::getFile (state); }
    void setFile (const juce::File& f, juce::UndoManager* um)    { FileRef::setFile (state, f, um); }

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

    // An audio generator plays files placed on its playlist row instead of
    // patterns, so it hosts no instrument at all -- its track carries wave
    // clips where every other type carries MIDI. It still has a fader, a pan
    // and an effect chain, which is why it is a generator type rather than a
    // separate kind of thing.
    static constexpr const char* audioType = "audio";

    bool isDrumKit() const  { return getType() == drumKitType; }
    bool isSampler() const  { return getType() == samplerType || isDrumKit(); }
    bool isAudio() const    { return getType() == audioType; }

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

    // The first slot holding no pattern, searching forward from the one after
    // `after` and wrapping round at D9. Duplicating a pattern lands in this,
    // so a copy appears next to what it was copied from rather than back at
    // A1. Empty only when all 36 slots are taken.
    std::optional<PatternSlot> findFreeSlot (std::optional<PatternSlot> after = {}) const;

    // Copies a pattern into one of this generator's slots: its length and its
    // notes, under a fresh id, so nothing that referenced the source now
    // references the copy. The source may belong to another generator, which
    // is all there is to copying a pattern between them.
    //
    // The destination slot must be free -- findFreeSlot is what picks one --
    // because overwriting a slot the user cannot see is not what "duplicate"
    // ever means.
    Pattern duplicatePattern (const Pattern& source, const PatternSlot& destination,
                              juce::UndoManager* um);

    // Materialises the slot the first time it is used. Empty slots stay out of
    // the tree deliberately: writing all 36 into every generator would bloat
    // every .carve and make every EditSync resync walk dead nodes.
    //
    // lengthBeats is what a fresh slot gets. Callers that know the song pass a
    // bar of its signature, so a 6/8 song doesn't open every pattern at a bar
    // and a third.
    Pattern getOrCreatePatternInSlot (const PatternSlot& slot, juce::UndoManager* um,
                                      double lengthBeats = Pattern::defaultLengthBeats);

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

// An audio file placed on an audio generator's row. Where a PlaylistClip
// names a pattern and inherits its length, this one names a file and states
// its length outright: there is no pattern to fall back on, and the source's
// own length is only known once something has read it off disk.
//
// Times are in beats, like everything else on the playlist. That does mean a
// tempo change slides the audio around rather than stretching it -- but the
// grid the user drops onto is a bar grid, so beats are what a placement is
// actually expressed in, and pitch-shifting is out of scope either way.
class AudioClip
{
public:
    explicit AudioClip (juce::ValueTree v) : state (std::move (v)) {}

    // A placement can be trimmed down to a tenth of a second. Short, because
    // this is a floor against a zero-length clip rather than a musical limit.
    static constexpr double minLengthSeconds = 0.1;

    juce::String getId() const           { return state[ids::id]; }
    juce::String getGeneratorId() const  { return state[ids::generatorId]; }

    // The source, stored the same way a sampler sound's is; see FileRef.
    juce::File getFile() const                                   { return FileRef::getFile (state); }
    void setFile (const juce::File& f, juce::UndoManager* um)    { FileRef::setFile (state, f, um); }

    // What the clip is called on the grid and on the track: the file's name,
    // because nothing here renames a placement.
    juce::String getName() const  { return getFile().getFileNameWithoutExtension(); }

    // Start is musical and length is not, on purpose. Where a clip sits is an
    // arrangement decision -- a vocal that enters at bar 9 should still enter
    // at bar 9 after a tempo change -- but how much of the file it plays is a
    // property of the file, and no tempo change can stretch it.
    double getStart() const           { return state[ids::start]; }
    double getLengthSeconds() const   { return std::max (minLengthSeconds, (double) state[ids::length]); }

    // Where in the source the placement starts, so a clip can play from part
    // way in. Everything reads and honours it, but no gesture writes one yet:
    // the grid only trims the end. Absent means "from the top", which is what
    // a dropped file gets.
    double getOffsetSeconds() const  { return std::max (0.0, (double) state.getProperty (ids::offset, 0.0)); }

    void setStart (double beats, juce::UndoManager* um)             { state.setProperty (ids::start, std::max (0.0, beats), um); }
    void setLengthSeconds (double seconds, juce::UndoManager* um)   { state.setProperty (ids::length, std::max (minLengthSeconds, seconds), um); }
    void setOffsetSeconds (double seconds, juce::UndoManager* um)   { state.setProperty (ids::offset, std::max (0.0, seconds), um); }

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

    // Audio placements live alongside the pattern ones as their own node type,
    // so getClips() keeps returning exactly what it always did.
    std::vector<AudioClip> getAudioClips() const;

    AudioClip addAudioClip (const Generator& generator, const juce::File& file,
                            double startBeats, double lengthSeconds, juce::UndoManager* um);
    void removeAudioClip (const AudioClip& clip, juce::UndoManager* um);

    juce::ValueTree state;
};


// Beats are quarter notes, as they are in tracktion, so a 6/8 bar is three of
// them rather than six.
struct TimeSignature
{
    int numerator = 4;
    int denominator = 4;

    double getBeatsPerBar() const  { return numerator * 4.0 / juce::jmax (1, denominator); }

    bool operator== (const TimeSignature&) const = default;
};

// A tempo change part way through the song. The song's own "tempo" is what
// plays until the first of these, so a song without any behaves exactly as it
// did before they existed.
class TempoChange
{
public:
    explicit TempoChange (juce::ValueTree v) : state (std::move (v)) {}

    double getStartBeat() const  { return state[ids::start]; }
    double getBpm() const        { return juce::jmax (1.0, (double) state[ids::bpm]); }

    void setStartBeat (double beat, juce::UndoManager* um)  { state.setProperty (ids::start, juce::jmax (0.0, beat), um); }
    void setBpm (double bpm, juce::UndoManager* um)         { state.setProperty (ids::bpm, juce::jlimit (20.0, 999.0, bpm), um); }

    juce::ValueTree state;
};

// A time signature change. Constrained to a bar line by whatever writes it --
// the model does not enforce it, because "which bar" is only meaningful once
// you have walked the changes before this one.
class TimeSigChange
{
public:
    explicit TimeSigChange (juce::ValueTree v) : state (std::move (v)) {}

    double getStartBeat() const  { return state[ids::start]; }

    TimeSignature getSignature() const
    {
        return { juce::jmax (1, (int) state[ids::numerator]),
                 juce::jmax (1, (int) state[ids::denominator]) };
    }

    void setStartBeat (double beat, juce::UndoManager* um)  { state.setProperty (ids::start, juce::jmax (0.0, beat), um); }

    void setSignature (TimeSignature sig, juce::UndoManager* um)
    {
        state.setProperty (ids::numerator, juce::jmax (1, sig.numerator), um);
        state.setProperty (ids::denominator, juce::jmax (1, sig.denominator), um);
    }

    juce::ValueTree state;
};

// The mix bus. It carries the same EFFECT nodes a generator does, so one
// effect implementation and one EditSync reconciliation cover both -- the only
// difference is which plugin list they end up in.
class MasterBus
{
public:
    explicit MasterBus (juce::ValueTree v) : state (std::move (v)) {}

    static constexpr float defaultVolumeDb = 0.0f;

    float getVolumeDb() const  { return state.getProperty (ids::volumeDb, defaultVolumeDb); }
    void setVolumeDb (float db, juce::UndoManager* um)  { state.setProperty (ids::volumeDb, db, um); }

    std::vector<Effect> getEffects() const;
    std::optional<Effect> findEffect (const juce::String& effectId) const;

    Effect addEffect (const juce::String& type, const juce::PluginDescription*, juce::UndoManager*);
    void removeEffect (const Effect&, juce::UndoManager*);
    void moveEffect (const Effect&, int newIndex, juce::UndoManager*);

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

    // Media paths -- sampler sounds and playlist audio clips -- which are the
    // only thing in the model that means anything outside the file. Resolving
    // fixes each one up against where the song is being loaded from, and
    // refreshing rewrites the relative half against where it is being saved
    // to; FileRef explains which of the two paths wins and why. loadFromFile
    // and saveToFile call these, and both do nothing at all to a song that
    // references no media.
    void resolveMediaPaths (const juce::File& songFile) const;
    void refreshMediaPaths (const juce::File& songFile) const;

    // EditSync matches an audio placement to the wave clip it already built by
    // id, so one without an id would be torn down and rebuilt on every resync
    // -- re-reading the file and cutting whatever it is playing. Everything
    // this code writes has an id; a hand-written song is the gap, and loading
    // is the moment to close it. Called by fromXml, so every load path gets it.
    void ensureAudioClipIds() const;

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

    // Materialised on first use, like the playlist, so songs written before the
    // master bus existed gain nothing until something touches it.
    MasterBus getMasterBus() const;

    // Where the last placement on the playlist ends, counting both pattern and
    // audio clips. This is what "the whole song" means to the transport, so it
    // has to live here rather than in whichever view happens to need it.
    double getLengthBeats() const;

    // Beats and seconds, at the song's one tempo. Placing an audio file needs
    // this: the file's length only exists in seconds.

    // Tempo and time signature changes, in beat order. Both lists are empty in
    // a song that never changes either, and the getters below fall back to the
    // song's own tempo and to 4/4 -- so nothing written before these existed
    // has to be migrated.
    std::vector<TempoChange> getTempoChanges() const;
    TempoChange addTempoChange (double startBeat, double bpm, juce::UndoManager*);
    void removeTempoChange (const TempoChange&, juce::UndoManager*);
    double getTempoAt (double beat) const;

    std::vector<TimeSigChange> getTimeSigChanges() const;
    TimeSigChange addTimeSigChange (double startBeat, TimeSignature, juce::UndoManager*);
    void removeTimeSigChange (const TimeSigChange&, juce::UndoManager*);
    TimeSignature getTimeSigAt (double beat) const;

    // Bars are 0-based here and displayed 1-based, like every other DAW.
    struct BarsAndBeats
    {
        int bar = 0;
        double beat = 0.0;
    };

    BarsAndBeats toBarsAndBeats (double beat) const;
    double beatOfBar (int bar) const;

    // Piecewise across the tempo changes: the only place in the app that turns
    // beats into wall-clock time, so a tempo change lands everywhere at once.
    double beatsFromSeconds (double seconds) const;
    double secondsFromBeats (double beats) const;

    // Whether anything on the playlist plays this pattern. Used to decide if a
    // slot the user only browsed past can be dropped again.
    bool isPatternUsedInPlaylist (const Pattern& pattern) const;

    juce::ValueTree state;
};

} // namespace carve::model
