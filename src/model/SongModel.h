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

// How close two tempo or time signature changes have to be to count as sharing
// a beat. The positions the playlist writes are computed -- a rounded beat, a
// bar line walked to from the start of the song -- so exact equality would let
// a second change land a rounding error away from the first.
inline constexpr double sameBeatTolerance = 1.0e-9;

inline bool isSameBeat (double a, double b)  { return std::abs (a - b) < sameBeatTolerance; }

class Note
{
public:
    explicit Note (juce::ValueTree v) : state (std::move (v)) {}

    // What a note may be, clamped here rather than only at each gesture that
    // writes one. A pitch outside 0..127 is not a MIDI note; velocity 0 is a
    // note off rather than a very quiet note, which is why the floor is 1; and
    // a note before the start of its pattern, or with no length, is never
    // heard. The piano roll already held all four lines -- see
    // PianoRollComponent::setNoteVelocity for the velocity one -- but MIDI
    // import, a pasted clipboard and a hand-written .carve all reach a note
    // without going past it.
    // Named for MIDI rather than for a keyboard: PianoRollComponent has its
    // own lowestPitch/highestPitch, which are the rows it draws (C1..C7) and a
    // good deal narrower than this.
    static constexpr int lowestMidiNote = 0;
    static constexpr int highestMidiNote = 127;
    static constexpr int quietestVelocity = 1;
    static constexpr int loudestVelocity = 127;

    // The absolute floor, below which a note is a click rather than a note.
    // Every gesture that makes one sits above it -- the piano roll draws no
    // shorter than a 1/32 (freeMinLengthBeats) and a MIDI import no shorter
    // than midiio::minNoteLengthBeats -- so this is what catches a note that
    // came from neither.
    static constexpr double minLengthBeats = 1.0 / 128.0;

    static double clampStart (double beats)   { return juce::jmax (0.0, beats); }
    static double clampLength (double beats)  { return juce::jmax (minLengthBeats, beats); }
    static int clampPitch (int midiNote)      { return juce::jlimit (lowestMidiNote, highestMidiNote, midiNote); }
    static int clampVelocity (int vel)        { return juce::jlimit (quietestVelocity, loudestVelocity, vel); }

    double getStart() const     { return state[ids::start]; }
    double getLength() const    { return state[ids::length]; }
    int getPitch() const        { return state[ids::pitch]; }
    int getVelocity() const     { return state[ids::velocity]; }

    void setStart (double beats, juce::UndoManager* um)   { state.setProperty (ids::start, clampStart (beats), um); }
    void setLength (double beats, juce::UndoManager* um)  { state.setProperty (ids::length, clampLength (beats), um); }
    void setPitch (int midiNote, juce::UndoManager* um)   { state.setProperty (ids::pitch, clampPitch (midiNote), um); }
    void setVelocity (int vel, juce::UndoManager* um)     { state.setProperty (ids::velocity, clampVelocity (vel), um); }

    juce::ValueTree state;
};

// A pattern is made when the user asks for one and carries a name from that
// moment on, so nothing has to be typed before drawing: patternStem below is
// what the automatic name is built from.
//
// The stem a generator's own new patterns are numbered from: the first two
// characters of its name ("Bass" -> "Ba", "Drum Kit 1" -> "Dr"), so a name
// stays as short to read as the old A1..D9 slot keys were while still saying
// which instrument it belongs to. Falls back to "Pt" for a nameless generator.
juce::String patternStemForGenerator (const juce::String& generatorName);

// The stem a copy is numbered from: the source's name with any trailing number
// taken off, so cloning "Ba1" offers "Ba2" rather than "Ba1 copy". A name that
// ends in no number keeps all of it and gains a space, so "Melody" clones to
// "Melody 2". Both stems already carry their separator -- a name is the stem
// with a number stuck straight on the end.
juce::String patternStemForCopy (const juce::String& patternName);

class Pattern
{
public:
    explicit Pattern (juce::ValueTree v) : state (std::move (v)) {}

    // What a new pattern gets when the caller has no song to ask: one bar of
    // 4/4.
    static constexpr double defaultLengthBeats = 4.0;

    // A sixteenth of a beat at the shortest. A zero or negative pattern has no
    // bars to draw, and PlaylistClip::getLength falls back to it, so it would
    // take every placement that inherits its length down with it.
    static constexpr double minLengthBeats = 1.0 / 16.0;

    static double clampLengthBeats (double beats)  { return juce::jmax (minLengthBeats, beats); }

    juce::String getId() const      { return state[ids::id]; }
    juce::String getName() const    { return state[ids::name]; }
    double getLengthBeats() const   { return state[ids::lengthBeats]; }

    void setName (const juce::String& n, juce::UndoManager* um)  { state.setProperty (ids::name, n, um); }

    void setLengthBeats (double beats, juce::UndoManager* um)
    {
        state.setProperty (ids::lengthBeats, clampLengthBeats (beats), um);
    }

    bool isEmpty() const  { return getNumNotes() == 0; }

    // Notes, and only notes: a PATTERN may hold other kinds of child, so
    // these three agree with each other rather than with getNumChildren().
    int getNumNotes() const;
    Note getNote (int index) const;
    std::vector<Note> getNotes() const;

    Note addNote (double startBeats, double lengthBeats, int pitch, int velocity, juce::UndoManager* um);
    void removeNote (const Note& note, juce::UndoManager* um);

    // Drops every note, keeping the pattern's id, name and length -- so
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

    // Whether the node names a file at all, and what that file is called.
    // Both read the stored strings rather than getFile(): a hand-written song
    // can hold a relative `file`, or a `relPath` alone, and getFile() gives
    // an empty File for either -- but the node still means a file, and a
    // search for it by name needs the name it was given.
    static bool hasFile (const juce::ValueTree&);
    static juce::String getFileName (const juce::ValueTree&);

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

    // Source excerpt in seconds; zero preserves whole-file playback for old songs.
    double getLengthSeconds() const { return state.getProperty (ids::length, 0.0); }
    void setLengthSeconds (double seconds, juce::UndoManager* um)
    {
        state.setProperty (ids::length, juce::jlimit (0.0, 36000.0, seconds), um);
    }

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

    // Which generator's audio feeds this effect's sidechain input, for effects
    // that have one (the compressor). Empty means none. Stored as a generator
    // id rather than anything engine-side, so it survives a reload; EditSync
    // resolves it to the live track each sync.
    juce::String getSidechainSourceId() const  { return state[ids::sidechainSource]; }

    void setSidechainSourceId (const juce::String& generatorId, juce::UndoManager* um)
    {
        if (generatorId.isEmpty())
            state.removeProperty (ids::sidechainSource, um);
        else
            state.setProperty (ids::sidechainSource, generatorId, um);
    }

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


// One point on an automation lane. The value is the parameter's own value --
// fader position for volume, -1..1 for pan -- because that is what tracktion's
// curves store, so nothing translates twice.
class AutomationPoint
{
public:
    explicit AutomationPoint (juce::ValueTree v) : state (std::move (v)) {}

    double getBeat() const   { return state[ids::beat]; }
    float getValue() const   { return state[ids::value]; }
    float getCurve() const   { return state.getProperty (ids::curve, 0.0f); }

    void setBeat (double beat, juce::UndoManager* um)   { state.setProperty (ids::beat, juce::jmax (0.0, beat), um); }
    void setValue (float value, juce::UndoManager* um)  { state.setProperty (ids::value, value, um); }
    void setCurve (float curve, juce::UndoManager* um)  { state.setProperty (ids::curve, juce::jlimit (-1.0f, 1.0f, curve), um); }

    juce::ValueTree state;
};

// A parameter's automation over the whole song. Positions are beats -- the
// engine wants seconds, but a curve stored in seconds would detach from the
// music at the first tempo change; EditSync converts on the way in.
class AutomationLane
{
public:
    explicit AutomationLane (juce::ValueTree v) : state (std::move (v)) {}

    // What the volume/pan targets are called; anything else is an effect id
    // (or instrumentTarget), with getParam() naming the parameter.
    static constexpr const char* volumeTarget = "volume";
    static constexpr const char* panTarget = "pan";
    static constexpr const char* instrumentTarget = "instrument";

    juce::String getTarget() const  { return state[ids::target]; }
    juce::String getParam() const   { return state[ids::param]; }

    int getNumPoints() const;
    std::vector<AutomationPoint> getPoints() const;   // beat order

    AutomationPoint addPoint (double beat, float value, juce::UndoManager*);
    void removePoint (const AutomationPoint&, juce::UndoManager*);

    juce::ValueTree state;
};


// A modulation source on a generator -- an LFO for now, the kind field there
// for the others tracktion ships (step, envelope follower, random). Its
// assignments say which parameters it drives, addressed the same way an
// automation lane is.
class ModifierAssign
{
public:
    explicit ModifierAssign (juce::ValueTree v) : state (std::move (v)) {}

    juce::String getTarget() const  { return state[ids::target]; }
    juce::String getParam() const   { return state[ids::param]; }
    float getAmount() const         { return state.getProperty (ids::amount, 1.0f); }

    void setAmount (float a, juce::UndoManager* um)  { state.setProperty (ids::amount, juce::jlimit (-1.0f, 1.0f, a), um); }

    juce::ValueTree state;
};

class GenModifier
{
public:
    explicit GenModifier (juce::ValueTree v) : state (std::move (v)) {}

    static constexpr const char* lfoKind = "lfo";

    juce::String getId() const    { return state[ids::id]; }
    juce::String getKind() const  { return state.getProperty (ids::kind, lfoKind); }

    // Mirrored one to one onto the LFOModifier's own properties, so the model
    // needs no opinion about their meaning -- rateType 3 is "bar", wave 0 is
    // sine, exactly as tracktion defines them.
    float getRate() const      { return state.getProperty (ids::rate, 1.0f); }
    float getRateType() const  { return state.getProperty (ids::rateType, 3.0f); }
    float getDepth() const     { return state.getProperty (ids::depth, 1.0f); }
    float getWave() const      { return state.getProperty (ids::wave, 0.0f); }
    float getSyncType() const  { return state.getProperty (ids::syncType, 0.0f); }
    bool isBipolar() const     { return state.getProperty (ids::bipolar, false); }
    float getPhase() const     { return state.getProperty (ids::phase, 0.0f); }
    float getOffset() const    { return state.getProperty (ids::offset, 0.0f); }

    void set (const juce::Identifier& property, const juce::var& value, juce::UndoManager* um)
    {
        state.setProperty (property, value, um);
    }

    std::vector<ModifierAssign> getAssigns() const;
    ModifierAssign addAssign (const juce::String& target, const juce::String& param, juce::UndoManager*);
    void removeAssign (const ModifierAssign&, juce::UndoManager*);

    juce::ValueTree state;
};

class Send;
class Return;

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

    // The built-in synth and the built-in drum machine. Both are internal
    // tracktion plugins, so their settings are kept the way an internal
    // effect's are: the plugin's own tree, under INSTRUMENT (see
    // getInternalState), rather than as an opaque blob.
    static constexpr const char* synthType = "internal-synth";
    static constexpr const char* drumSynthType = "drum-synth";

    bool isDrumKit() const    { return getType() == drumKitType; }
    bool isDrumSynth() const  { return getType() == drumSynthType; }
    bool isInternalInstrument() const  { return getType() == synthType || isDrumSynth(); }
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

    // For an internal instrument (4OSC, the drum synth): its plugin's own
    // ValueTree, captured on save and handed back to the plugin cache on
    // load, the same way an internal effect keeps its settings. Kept under an
    // INSTRUMENT node of our own so it can never be mistaken for the PLUGIN
    // node that describes an external instrument.
    juce::ValueTree getInternalState() const;
    void setInternalState (const juce::ValueTree& pluginState, juce::UndoManager*);

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

    // Modulation sources and what they drive.
    std::vector<GenModifier> getModifiers() const;
    std::optional<GenModifier> findModifier (const juce::String& modifierId) const;
    GenModifier addModifier (const juce::String& kind, juce::UndoManager*);
    void removeModifier (const GenModifier&, juce::UndoManager*);

    // Automation lanes, one per automated parameter. findLane answers by
    // (target, param); addLane materialises on first use.
    std::vector<AutomationLane> getAutomationLanes() const;
    std::optional<AutomationLane> findAutomationLane (const juce::String& target, const juce::String& param) const;
    AutomationLane addAutomationLane (const juce::String& target, const juce::String& param, juce::UndoManager*);
    void removeAutomationLane (const AutomationLane&, juce::UndoManager*);

    // Sends into return busses. findSend answers by return id; setSendGain
    // materialises the SEND node on first use, so an untouched send stays out
    // of the file.
    std::vector<Send> getSends() const;
    std::optional<Send> findSend (const juce::String& returnId) const;
    Send setSendGain (const juce::String& returnId, float gainDb, juce::UndoManager*);
    void removeSend (const juce::String& returnId, juce::UndoManager*);

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

    // `stem` with the first number after it that no pattern of this generator
    // is already called. What both createPattern and duplicatePattern name
    // their result, so neither ever has to ask the user for one.
    juce::String makeUniquePatternName (const juce::String& stem) const;

    // A new, empty, automatically named pattern, appended to the list.
    //
    // lengthBeats is what it opens at. Callers that know the song pass a bar
    // of its signature, so a 6/8 song doesn't open every pattern at a bar and
    // a third.
    Pattern createPattern (juce::UndoManager* um,
                           double lengthBeats = Pattern::defaultLengthBeats);

    // Copies a pattern into this generator: its length and its notes, under a
    // fresh id and an automatic name, so nothing that referenced the source
    // now references the copy. The source may belong to another generator,
    // which is all there is to copying a pattern between them.
    //
    // A copy of one of this generator's own patterns lands directly after it,
    // so a chain of clones reads in order in the picker; one from elsewhere
    // has nothing to sit next to and goes on the end.
    Pattern duplicatePattern (const Pattern& source, juce::UndoManager* um);

    // Appends a pattern under a name the caller chose. createPattern is what
    // the UI uses; this is for the places that have a name already -- the demo
    // song and a MIDI import.
    Pattern addPattern (const juce::String& name, double lengthBeats, juce::UndoManager* um);
    void removePattern (const Pattern& pattern, juce::UndoManager* um);

    juce::ValueTree state;

private:
    // index < 0 appends. The one place a PATTERN node is built, so addPattern
    // and duplicatePattern cannot drift apart on what one is made of.
    Pattern insertPattern (int index, const juce::String& name, double lengthBeats,
                           juce::UndoManager* um);
};

class PlaylistClip
{
public:
    explicit PlaylistClip (juce::ValueTree v) : state (std::move (v)) {}

    juce::String getId() const           { return state[ids::id]; }
    juce::String getGeneratorId() const  { return state[ids::generatorId]; }
    juce::String getPatternId() const    { return state[ids::patternId]; }
    double getStart() const              { return state[ids::start]; }

    // A placement of its own is at least this long: a trim may cut a clip down
    // but never away. The same number as Pattern::minLengthBeats, and by
    // coincidence rather than by rule -- one is how short a pattern may be,
    // the other how short a placement of one may be.
    static constexpr double minLengthBeats = 1.0 / 16.0;

    // Four octaves either way. Past that a transposed pattern runs off the
    // ends of the MIDI range rather than sounding lower or higher.
    static constexpr int maxTransposeSemitones = 48;

    static double clampStart (double beats)    { return juce::jmax (0.0, beats); }
    static double clampLength (double beats)   { return juce::jmax (minLengthBeats, beats); }
    static int clampTranspose (int semitones)  { return juce::jlimit (-maxTransposeSemitones, maxTransposeSemitones, semitones); }

    // Clamped the way an AudioClip's is: a placement before the start of the
    // song is never played and never drawn, so the two kinds of placement had
    // no business disagreeing about it.
    void setStart (double beats, juce::UndoManager* um)  { state.setProperty (ids::start, clampStart (beats), um); }

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
        state.setProperty (ids::length, clampLength (beats), um);
    }

    void clearLength (juce::UndoManager* um)  { state.removeProperty (ids::length, um); }

    // Semitones added to every note of the pattern, for this placement only.
    int getTranspose() const  { return state.getProperty (ids::transpose, 0); }

    void setTranspose (int semitones, juce::UndoManager* um)
    {
        state.setProperty (ids::transpose, clampTranspose (semitones), um);
    }

    juce::ValueTree state;
};

// What a pattern placement says about itself beyond where it sits and what it
// plays, for the callers that build one from an existing clip -- a duplicate,
// a paste. No length means "as long as the pattern" and no transpose means "as
// written", which is exactly what a fresh clip has, so a copy of an ordinary
// clip stays identical in the saved file to the clip it came from.
struct ClipPlacement
{
    std::optional<double> length;
    int transpose = 0;

    // Everything the source clip states outright, ready to place elsewhere.
    // Deliberately not a copy of its tree: a paste may retarget the clip at
    // another generator's pattern, and the clipboard carries scratch
    // attributes that have no business in the document.
    static ClipPlacement of (const PlaylistClip& source)
    {
        ClipPlacement placement;
        placement.transpose = source.getTranspose();

        // A stored length of zero or less means "inherit" just as an absent
        // one does -- see getLength -- so it is carried over as absent, rather
        // than clamped up into a sixteenth-of-a-beat clip.
        if (const double stored = source.state.getProperty (ids::length, 0.0); stored > 0.0)
            placement.length = stored;

        return placement;
    }
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

    // Pattern placements only. The audio ones are children of this same node
    // and are reached through getAudioClips(), so neither count includes the
    // other.
    int getNumClips() const;
    PlaylistClip getClip (int index) const;
    std::vector<PlaylistClip> getClips() const;

    PlaylistClip addClip (const Generator& generator, const Pattern& pattern,
                          double startBeats, juce::UndoManager* um);

    // The same, for a placement that does not simply inherit its pattern's
    // length. One write rather than an add followed by a setLength and a
    // setTranspose: each of those re-entered every listener that rebuilds
    // itself off the playlist, and the states in between were a clip that
    // played the wrong thing for the wrong length.
    PlaylistClip addClip (const Generator& generator, const Pattern& pattern,
                          double startBeats, const ClipPlacement& placement,
                          juce::UndoManager* um);

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

    // What a tempo may be, here rather than only in the spinner that edits
    // one: a song's tempo also arrives from disk, and every beats-to-seconds
    // walk divides by it.
    static constexpr double minBpm = 20.0;
    static constexpr double maxBpm = 999.0;

    static double clampBpm (double bpm)  { return juce::jlimit (minBpm, maxBpm, bpm); }

    double getStartBeat() const  { return state[ids::start]; }
    double getBpm() const        { return juce::jmax (1.0, (double) state[ids::bpm]); }

    // Clamped at zero, and refused outright if another change already owns the
    // beat -- so one change per beat holds on every path that writes one, not
    // only on the one that makes one. See Song::addTempoChange for why.
    void setStartBeat (double beat, juce::UndoManager*);

    void setBpm (double bpm, juce::UndoManager* um)  { state.setProperty (ids::bpm, clampBpm (bpm), um); }

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

    // Clamped and refused the same way TempoChange::setStartBeat is.
    void setStartBeat (double beat, juce::UndoManager*);

    void setSignature (TimeSignature sig, juce::UndoManager* um)
    {
        state.setProperty (ids::numerator, juce::jmax (1, sig.numerator), um);
        state.setProperty (ids::denominator, juce::jmax (1, sig.denominator), um);
    }

    juce::ValueTree state;
};

// One generator's send into a return bus. At most one per (generator, return);
// identity is the returnId.
class Send
{
public:
    explicit Send (juce::ValueTree v) : state (std::move (v)) {}

    static constexpr float defaultGainDb = -12.0f;

    juce::String getReturnId() const  { return state[ids::returnId]; }
    float getGainDb() const           { return state.getProperty (ids::gainDb, defaultGainDb); }

    void setGainDb (float db, juce::UndoManager* um)  { state.setProperty (ids::gainDb, juce::jlimit (-60.0f, 12.0f, db), um); }

    juce::ValueTree state;
};

// A shared effect bus: generators send into it by bus number, and the return
// track carries the shared effects. The bus number is allocated once and
// saved, so the send/return pairing survives a reload.
class Return
{
public:
    explicit Return (juce::ValueTree v) : state (std::move (v)) {}

    juce::String getId() const    { return state[ids::id]; }
    juce::String getName() const  { return state[ids::name]; }
    int getBusNumber() const      { return state[ids::busNumber]; }
    float getVolumeDb() const     { return state.getProperty (ids::volumeDb, 0.0f); }
    bool isMuted() const          { return state.getProperty (ids::mute, false); }

    void setName (const juce::String& n, juce::UndoManager* um)  { state.setProperty (ids::name, n, um); }
    void setVolumeDb (float db, juce::UndoManager* um)           { state.setProperty (ids::volumeDb, db, um); }
    void setMuted (bool m, juce::UndoManager* um)                { state.setProperty (ids::mute, m, um); }

    std::vector<Effect> getEffects() const;
    std::optional<Effect> findEffect (const juce::String& effectId) const;
    Effect addEffect (const juce::String& type, const juce::PluginDescription*, juce::UndoManager*);
    void removeEffect (const Effect&, juce::UndoManager*);
    void moveEffect (const Effect&, int newIndex, juce::UndoManager*);

    juce::ValueTree state;
};

// The mix bus. It carries the same EFFECT nodes a generator does, so one
// effect implementation and one EditSync reconciliation cover both -- the only
// difference is which plugin list they end up in.
class MasterBus
{
public:
    explicit MasterBus (juce::ValueTree v) : state (std::move (v)) {}

    // Matches the -3dB tracktion gives a fresh Edit's master fader: headroom
    // against summing, and -- more to the point -- what every song rendered at
    // before the master bus was model-backed. A 0dB default here quietly made
    // every existing song 3dB louder.
    static constexpr float defaultVolumeDb = -3.0f;

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

    // The media nodes -- the same sounds and audio clips -- that name a file
    // which is not on disk, in document order. Nothing here can play them, so
    // the app lists them for the user to point at their new home; a node with
    // no file at all is not missing, it is empty.
    std::vector<juce::ValueTree> findMissingMedia() const;

    // Looks through `folder` and everything under it for a file with each
    // missing node's name and points the node at the first one found, through
    // FileRef so the next save stores the relative path as well. Returns how
    // many nodes were relinked. Matching by name alone is deliberate: a
    // project moved with its media into a different layout, or a sample
    // library moved wholesale, keeps its file names and loses everything
    // else, and a wrong same-named file is one Locate... away from right.
    int relinkMissingMedia (const juce::File& folder, juce::UndoManager*) const;

    // EditSync matches a placement to the clip it already built by id, so one
    // without an id would be torn down and rebuilt on every resync -- an audio
    // placement re-reading its file, a pattern placement cutting the notes it
    // is playing. Everything this code writes has an id; a hand-written song
    // is the gap, and loading is the moment to close it. Called by fromXml, so
    // every load path gets it.
    void ensureClipIds() const;

    // Patterns used to live in a fixed A1..D9 grid, and a song saved then
    // carries a `slot` key on each of them. Nothing reads it any more -- the
    // pattern's name is its identity now, and for those songs that name is
    // still the old slot key, which is exactly the label they had. Dropping
    // the property on load keeps it from riding along in every later save.
    // Called by fromXml, so every load path gets it.
    void dropPatternSlots() const;

    // Drops any tempo or time signature change sharing a beat with another,
    // keeping the *last* in document order. That is the one that was in force:
    // getTempoAt and secondsFromBeats both walk the stable-sorted list writing
    // over what they have as they go, so of a pair on one beat the later one
    // is what the song sounded like. Loading therefore leaves the time axis
    // exactly where it was and only drops what nothing could hear.
    //
    // addTempoChange and addTimeSigChange refuse to make such a pair, so
    // nothing this code writes needs it; a hand-edited or merged .carve is the
    // gap. Called by fromXml, so every load path gets it.
    void dropDuplicateChanges() const;

    // Puts every value in the document back inside the range its setter would
    // have clamped it to: note pitches, velocities, starts and lengths,
    // pattern and placement lengths, the song's own tempo and loop range.
    //
    // The setters are where a value the app chooses is held to its range, and
    // that covers everything the app itself can write. A .carve is the other
    // way in, and nothing on that path had ever been past a setter -- so this
    // is the same rule applied at the same boundary dropDuplicateChanges is.
    // Called by fromXml too.
    void clampValues() const;

    juce::String getName() const  { return state[ids::name]; }
    // The tempo until the first tempo change, clamped the same way one of
    // those is. The floor on the way out matches TempoChange::getBpm's, and
    // for the same reason: secondsFromBeats divides by whatever comes back, so
    // a song arriving from disk with no tempo at all must not make it
    // infinite.
    double getTempo() const       { return juce::jmax (1.0, (double) state[ids::tempo]); }

    void setName (const juce::String& n, juce::UndoManager* um)  { state.setProperty (ids::name, n, um); }
    void setTempo (double bpm, juce::UndoManager* um)            { state.setProperty (ids::tempo, TempoChange::clampBpm (bpm), um); }

    // Playback loop range in beats, set from the playlist ruler. An empty
    // range means "loop the whole song", which is what playback did before
    // the range existed, so older songs keep their behaviour. Both ends are
    // clamped at zero and ordered, so no drag can leave an inverted range.
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

    // Takes the generator and everything that would otherwise point at
    // nothing: its clips on the playlist, and any effect sidechained to it.
    // One transaction's worth, so undo brings the lot back.
    void removeGenerator (const Generator&, juce::UndoManager*);

    Playlist getPlaylist() const;

    // Materialised on first use, like the playlist, so songs written before the
    // master bus existed gain nothing until something touches it.
    MasterBus getMasterBus() const;

    std::vector<Return> getReturns() const;
    std::optional<Return> findReturn (const juce::String& returnId) const;
    Return addReturn (const juce::String& name, juce::UndoManager*);

    // Takes the sends into it with it. A send names its return by id, so one
    // left behind names nothing: EditSync has to recognise it and drop the
    // live AuxSend, and the dead node rides along in every save from then on.
    void removeReturn (const Return&, juce::UndoManager*);

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

    // The change on this beat, if there is one. At most one can be: see
    // addTempoChange.
    std::optional<TempoChange> findTempoChangeAt (double beat) const;

    // One change per beat: two of a kind on one beat would fight over which of
    // them wins, and which won would be decided by nothing the user can see.
    // So a change on a beat that already has one is not made, and the one
    // already there is returned untouched -- the same shape addAutomationLane
    // and addModifier's addAssign use. Callers that mean "set the tempo here"
    // therefore set the bpm on what comes back.
    //
    // The beat is clamped at zero first, so two changes aimed at different
    // positions before the start of the song cannot both pass and then pile up
    // on beat 0.
    TempoChange addTempoChange (double startBeat, double bpm, juce::UndoManager*);
    void removeTempoChange (const TempoChange&, juce::UndoManager*);
    double getTempoAt (double beat) const;

    std::vector<TimeSigChange> getTimeSigChanges() const;
    std::optional<TimeSigChange> findTimeSigChangeAt (double beat) const;
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

    // Whether anything on the playlist plays this pattern. What the pattern
    // picker warns with before it deletes one.
    bool isPatternUsedInPlaylist (const Pattern& pattern) const;

    juce::ValueTree state;
};

} // namespace carve::model
