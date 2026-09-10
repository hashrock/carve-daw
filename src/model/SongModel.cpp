#include "SongModel.h"

#include <algorithm>
#include <cmath>
#include <map>

#include <juce_audio_formats/juce_audio_formats.h>

namespace carve::model
{

// tracktion names a plugin's state tree "PLUGIN"; spelled out here so the model
// layer doesn't have to include the engine just for one identifier.
static const juce::Identifier te_pluginStateType ("PLUGIN");

// Our limiter's xmlTypeName. Spelled out rather than included, so the model
// stays clear of the plugins: this file already knows effect types as strings
// (that is what the .carve stores), and src/model deliberately does not
// depend on src/plugins.
static const char* limiterEffectType = "carveLimiter";

namespace
{
    juce::String newId()  { return juce::Uuid().toString(); }

    juce::ValueTree getOrCreateChild (juce::ValueTree parent, const juce::Identifier& type)
    {
        auto child = parent.getChildWithName (type);
        if (! child.isValid())
        {
            child = juce::ValueTree (type);
            parent.appendChild (child, nullptr);
        }
        return child;
    }

    template <typename Wrapper>
    std::vector<Wrapper> collectChildren (const juce::ValueTree& parent, const juce::Identifier& type)
    {
        std::vector<Wrapper> result;
        for (const auto& child : parent)
            if (child.hasType (type))
                result.push_back (Wrapper (child));
        return result;
    }

    // The counted-and-indexed half of collectChildren, for the accessors that
    // walk a node without building a vector. Both filter by type on purpose:
    // several of these nodes already hold more than one kind of child (a
    // playlist holds CLIP and AUDIOCLIP), and the ones that do not are only
    // one feature away from it -- an automation lane under a PATTERN would
    // otherwise be counted as a note.
    int countChildren (const juce::ValueTree& parent, const juce::Identifier& type)
    {
        int count = 0;
        for (const auto& child : parent)
            if (child.hasType (type))
                ++count;
        return count;
    }

    // An invalid tree for an index past the end, which the wrappers turn into
    // an object whose getters all return their default -- the same thing
    // ValueTree::getChild does, rather than a new way to fail.
    juce::ValueTree getChildOfType (const juce::ValueTree& parent, const juce::Identifier& type, int index)
    {
        if (index >= 0)
            for (const auto& child : parent)
                if (child.hasType (type) && index-- == 0)
                    return child;

        return {};
    }
} // namespace

//==============================================================================
// Automatic pattern names

juce::String patternStemForGenerator (const juce::String& generatorName)
{
    const auto trimmed = generatorName.trim();

    // Two characters, or all of them if there are fewer. A generator with no
    // name at all still has to number its patterns from something.
    return trimmed.isEmpty() ? juce::String ("Pt") : trimmed.substring (0, 2);
}

juce::String patternStemForCopy (const juce::String& patternName)
{
    const auto trimmed = patternName.trim();

    if (trimmed.isEmpty())
        return "Pt";

    int end = trimmed.length();
    while (end > 0 && juce::CharacterFunctions::isDigit (trimmed[end - 1]))
        --end;

    // All digits ("12"): there is no stem to keep, so it numbers like a
    // nameless pattern rather than coming back as an empty name.
    if (end == 0)
        return "Pt";

    // A name that ended in a number keeps whatever separator it already had,
    // so "Ba1" clones to "Ba2" and "Bass 1" to "Bass 2". One that ended in
    // anything else gains a space, so "Melody" clones to "Melody 2" and not
    // to "Melody2".
    return end < trimmed.length() ? trimmed.substring (0, end)
                                  : trimmed + " ";
}

//==============================================================================
// Pattern

int Pattern::getNumNotes() const
{
    return countChildren (state, ids::NOTE);
}

Note Pattern::getNote (int index) const
{
    return Note (getChildOfType (state, ids::NOTE, index));
}

std::vector<Note> Pattern::getNotes() const
{
    return collectChildren<Note> (state, ids::NOTE);
}

Note Pattern::addNote (double startBeats, double lengthBeats, int pitch, int velocity, juce::UndoManager* um)
{
    // Through the same clamps the setters use: a note that arrives wrong from
    // a MIDI file or a clipboard should be as impossible as one edited wrong.
    juce::ValueTree note (ids::NOTE);
    note.setProperty (ids::start, Note::clampStart (startBeats), nullptr);
    note.setProperty (ids::length, Note::clampLength (lengthBeats), nullptr);
    note.setProperty (ids::pitch, Note::clampPitch (pitch), nullptr);
    note.setProperty (ids::velocity, Note::clampVelocity (velocity), nullptr);
    state.appendChild (note, um);
    return Note (note);
}

void Pattern::removeNote (const Note& note, juce::UndoManager* um)
{
    state.removeChild (note.state, um);
}

void Pattern::clearNotes (juce::UndoManager* um)
{
    // Backwards, because removing a child shuffles everything after it down.
    for (int i = state.getNumChildren(); --i >= 0;)
        if (state.getChild (i).hasType (ids::NOTE))
            state.removeChild (i, um);
}

void Pattern::copyNotesFrom (const Pattern& source, juce::UndoManager* um)
{
    if (source.state == state)
        return;

    clearNotes (um);

    // createCopy rather than the note's four properties: a NOTE that grows a
    // fifth one later should copy without anyone having to remember this.
    for (const auto& note : source.getNotes())
        state.appendChild (note.state.createCopy(), um);
}


//==============================================================================
// FileRef

juce::File FileRef::getFile (const juce::ValueTree& state)
{
    const auto path = state[ids::file].toString();

    // juce::File asserts on anything that isn't an absolute path, and a
    // hand-written song (or one whose paths were never resolved) can hold
    // whatever it likes here.
    return juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
}

void FileRef::setFile (juce::ValueTree state, const juce::File& file, juce::UndoManager* um)
{
    state.setProperty (ids::file, file.getFullPathName(), um);

    // Stale the moment the file changes; Song::saveToFile writes a fresh one
    // against whichever .carve the song ends up in.
    state.removeProperty (ids::relPath, um);
}

bool FileRef::hasFile (const juce::ValueTree& state)
{
    return state[ids::file].toString().isNotEmpty() || state[ids::relPath].toString().isNotEmpty();
}

juce::String FileRef::getFileName (const juce::ValueTree& state)
{
    // The absolute path when there is one, otherwise the relative one: the
    // name is the same in both, and either may be all a song has.
    auto path = state[ids::file].toString();

    if (path.isEmpty())
        path = state[ids::relPath].toString();

    // Split by hand rather than through juce::File, which asserts on a
    // relative path; a hand-written song can hold either separator.
    return path.fromLastOccurrenceOf ("/", false, false)
               .fromLastOccurrenceOf ("\\", false, false);
}

void FileRef::resolve (juce::ValueTree state, const juce::File& songDirectory)
{
    const auto relative = state[ids::relPath].toString();

    if (relative.isEmpty())
        return;

    if (const auto resolved = songDirectory.getChildFile (relative); resolved.existsAsFile())
        state.setProperty (ids::file, resolved.getFullPathName(), nullptr);
}

void FileRef::refresh (juce::ValueTree state, const juce::File& songDirectory)
{
    const auto file = getFile (state);

    if (file == juce::File())
        return;

    state.setProperty (ids::relPath, file.getRelativePathFrom (songDirectory), nullptr);
}

double readAudioFileLengthSeconds (const juce::File& file)
{
    if (! file.existsAsFile())
        return 0.0;

    // Built here rather than kept around: this runs once per file the user
    // drops, and a static one would be a shutdown-ordering problem for no gain.
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0)
        return 0.0;

    return (double) reader->lengthInSamples / reader->sampleRate;
}

//==============================================================================
// SamplerSound

void SamplerSound::setKeyRange (int lowest, int highest, juce::UndoManager* um)
{
    state.setProperty (ids::minNote, juce::jlimit (lowestNote, highestNote, std::min (lowest, highest)), um);
    state.setProperty (ids::maxNote, juce::jlimit (lowestNote, highestNote, std::max (lowest, highest)), um);
}

std::vector<SamplerSound> Generator::getSounds() const
{
    return collectChildren<SamplerSound> (state.getChildWithName (ids::SOUNDS), ids::SOUND);
}

SamplerSound Generator::addSound (const juce::File& file, juce::UndoManager* um)
{
    juce::ValueTree sound (ids::SOUND);
    sound.setProperty (ids::id, newId(), nullptr);
    sound.setProperty (ids::name, file.getFileNameWithoutExtension(), nullptr);

    getOrCreateChild (state, ids::SOUNDS).appendChild (sound, um);

    SamplerSound wrapper (sound);
    wrapper.setFile (file, um);
    return wrapper;
}

void Generator::removeSound (const SamplerSound& sound, juce::UndoManager* um)
{
    state.getChildWithName (ids::SOUNDS).removeChild (sound.state, um);
}

SamplerSound Generator::setSingleSound (const juce::File& file, juce::UndoManager* um)
{
    // Whole-keyboard replacement: it drops every existing sound. That is right
    // for a sampler and catastrophic for a drum kit, where each sound is a pad.
    jassert (! isDrumKit());

    if (isDrumKit())
        return addSound (file, um);

    for (const auto& existing : getSounds())
        removeSound (existing, um);

    return addSound (file, um);
}

//==============================================================================
// Effect

void Effect::setPlugin (const juce::PluginDescription& description, juce::UndoManager* um)
{
    if (auto xml = description.createXml())
        state.setProperty (ids::desc, xml->toString(), um);
}

std::optional<juce::PluginDescription> Effect::getPluginDescription() const
{
    if (auto xml = juce::parseXML (state[ids::desc].toString()))
    {
        juce::PluginDescription description;
        if (description.loadFromXml (*xml))
            return description;
    }
    return std::nullopt;
}

juce::ValueTree Effect::getInternalState() const
{
    return state.getChildWithName (te_pluginStateType);
}

void Effect::setInternalState (const juce::ValueTree& pluginState, juce::UndoManager* um)
{
    state.removeChild (state.getChildWithName (te_pluginStateType), um);

    if (pluginState.isValid())
        state.appendChild (pluginState.createCopy(), um);
}

std::vector<Effect> Generator::getEffects() const
{
    return collectChildren<Effect> (state.getChildWithName (ids::EFFECTS), ids::EFFECT);
}

std::optional<Effect> Generator::findEffect (const juce::String& effectId) const
{
    auto found = state.getChildWithName (ids::EFFECTS)
                      .getChildWithProperty (ids::id, effectId);
    if (! found.isValid())
        return std::nullopt;

    return Effect (found);
}

Effect Generator::addEffect (const juce::String& type,
                             const juce::PluginDescription* description,
                             juce::UndoManager* um)
{
    juce::ValueTree effect (ids::EFFECT);
    effect.setProperty (ids::id, newId(), nullptr);
    effect.setProperty (ids::type, type, nullptr);

    getOrCreateChild (state, ids::EFFECTS).appendChild (effect, um);

    Effect wrapper (effect);
    if (description != nullptr)
        wrapper.setPlugin (*description, um);

    return wrapper;
}

void Generator::removeEffect (const Effect& effect, juce::UndoManager* um)
{
    state.getChildWithName (ids::EFFECTS).removeChild (effect.state, um);
}

void Generator::moveEffect (const Effect& effect, int newIndex, juce::UndoManager* um)
{
    auto effects = state.getChildWithName (ids::EFFECTS);
    const auto from = effects.indexOf (effect.state);

    if (from < 0)
        return;

    effects.moveChild (from, juce::jlimit (0, effects.getNumChildren() - 1, newIndex), um);
}

//==============================================================================
// Generator

int Generator::getNumPatterns() const
{
    return state.getChildWithName (ids::PATTERNS).getNumChildren();
}

Pattern Generator::getPattern (int index) const
{
    return Pattern (state.getChildWithName (ids::PATTERNS).getChild (index));
}

std::vector<Pattern> Generator::getPatterns() const
{
    return collectChildren<Pattern> (state.getChildWithName (ids::PATTERNS), ids::PATTERN);
}

std::optional<Pattern> Generator::findPattern (const juce::String& patternId) const
{
    auto found = state.getChildWithName (ids::PATTERNS)
                      .getChildWithProperty (ids::id, patternId);
    if (! found.isValid())
        return std::nullopt;
    return Pattern (found);
}

juce::String Generator::makeUniquePatternName (const juce::String& stem) const
{
    const auto patterns = getPatterns();

    // Linear in the pattern count squared, and deliberately: a generator holds
    // a handful of patterns, and the first free number is what makes a name
    // predictable -- deleting "Ba2" and asking for another gives "Ba2" back
    // rather than "Ba5".
    for (int number = 1; ; ++number)
    {
        const auto candidate = stem + juce::String (number);

        if (std::none_of (patterns.begin(), patterns.end(),
                          [&] (const Pattern& p) { return p.getName() == candidate; }))
            return candidate;
    }
}

Pattern Generator::createPattern (juce::UndoManager* um, double lengthBeats)
{
    return addPattern (makeUniquePatternName (patternStemForGenerator (getName())),
                       lengthBeats, um);
}

Pattern Generator::duplicatePattern (const Pattern& source, juce::UndoManager* um)
{
    auto patterns = getOrCreateChild (state, ids::PATTERNS);

    // A copy of one of ours goes straight after the original; one from another
    // generator has nothing to sit next to, so it goes on the end.
    const int sourceIndex = patterns.indexOf (source.state);
    const int index = sourceIndex >= 0 ? sourceIndex + 1 : -1;

    auto copy = insertPattern (index, makeUniquePatternName (patternStemForCopy (source.getName())),
                               source.getLengthBeats(), um);
    copy.copyNotesFrom (source, um);
    return copy;
}

Pattern Generator::addPattern (const juce::String& name, double lengthBeats, juce::UndoManager* um)
{
    return insertPattern (-1, name, lengthBeats, um);
}

Pattern Generator::insertPattern (int index, const juce::String& name, double lengthBeats,
                                  juce::UndoManager* um)
{
    juce::ValueTree pattern (ids::PATTERN);
    pattern.setProperty (ids::id, newId(), nullptr);
    pattern.setProperty (ids::name, name, nullptr);
    pattern.setProperty (ids::lengthBeats, juce::jmax (Pattern::minLengthBeats, lengthBeats), nullptr);
    getOrCreateChild (state, ids::PATTERNS).addChild (pattern, index, um);
    return Pattern (pattern);
}

void Generator::removePattern (const Pattern& pattern, juce::UndoManager* um)
{
    state.getChildWithName (ids::PATTERNS).removeChild (pattern.state, um);
}

void Generator::setPlugin (const juce::PluginDescription& description, juce::UndoManager* um)
{
    auto plugin = getOrCreateChild (state, ids::PLUGIN);
    if (auto xml = description.createXml())
        plugin.setProperty (ids::desc, xml->toString(), um);
}

std::optional<juce::PluginDescription> Generator::getPluginDescription() const
{
    auto plugin = state.getChildWithName (ids::PLUGIN);
    if (! plugin.isValid())
        return std::nullopt;

    if (auto xml = juce::parseXML (plugin[ids::desc].toString()))
    {
        juce::PluginDescription description;
        if (description.loadFromXml (*xml))
            return description;
    }
    return std::nullopt;
}

void Generator::setPluginState (const juce::String& base64, juce::UndoManager* um)
{
    getOrCreateChild (state, ids::PLUGIN).setProperty (ids::state, base64, um);
}

juce::String Generator::getPluginState() const
{
    return state.getChildWithName (ids::PLUGIN)[ids::state];
}

juce::ValueTree Generator::getInternalState() const
{
    return state.getChildWithName (ids::INSTRUMENT).getChildWithName (te_pluginStateType);
}

void Generator::setInternalState (const juce::ValueTree& pluginState, juce::UndoManager* um)
{
    state.removeChild (state.getChildWithName (ids::INSTRUMENT), um);

    if (pluginState.isValid())
    {
        juce::ValueTree holder (ids::INSTRUMENT);
        holder.appendChild (pluginState.createCopy(), nullptr);
        state.appendChild (holder, um);
    }
}

//==============================================================================
// Playlist

int Playlist::getNumClips() const
{
    return countChildren (state, ids::CLIP);
}

PlaylistClip Playlist::getClip (int index) const
{
    return PlaylistClip (getChildOfType (state, ids::CLIP, index));
}

std::vector<PlaylistClip> Playlist::getClips() const
{
    return collectChildren<PlaylistClip> (state, ids::CLIP);
}

PlaylistClip Playlist::addClip (const Generator& generator, const Pattern& pattern,
                                double startBeats, juce::UndoManager* um)
{
    return addClip (generator, pattern, startBeats, {}, um);
}

PlaylistClip Playlist::addClip (const Generator& generator, const Pattern& pattern,
                                double startBeats, const ClipPlacement& placement,
                                juce::UndoManager* um)
{
    juce::ValueTree clip (ids::CLIP);
    clip.setProperty (ids::id, newId(), nullptr);
    clip.setProperty (ids::generatorId, generator.getId(), nullptr);
    clip.setProperty (ids::patternId, pattern.getId(), nullptr);
    clip.setProperty (ids::start, PlaylistClip::clampStart (startBeats), nullptr);

    // Through the setters' clamps, and only when the placement has something
    // to say: an absent length and a zero transpose are what a plain clip
    // has, so writing them out unconditionally would put properties into
    // every song that never had them.
    if (placement.length)
        clip.setProperty (ids::length, PlaylistClip::clampLength (*placement.length), nullptr);

    if (placement.transpose != 0)
        clip.setProperty (ids::transpose, PlaylistClip::clampTranspose (placement.transpose), nullptr);

    // The tree is complete before it is attached, so everything listening to
    // the playlist sees the finished placement on its first callback.
    state.appendChild (clip, um);
    return PlaylistClip (clip);
}

void Playlist::removeClip (const PlaylistClip& clip, juce::UndoManager* um)
{
    state.removeChild (clip.state, um);
}

std::vector<AudioClip> Playlist::getAudioClips() const
{
    return collectChildren<AudioClip> (state, ids::AUDIOCLIP);
}

AudioClip Playlist::addAudioClip (const Generator& generator, const juce::File& file,
                                  double startBeats, double lengthSeconds, juce::UndoManager* um)
{
    juce::ValueTree clip (ids::AUDIOCLIP);
    clip.setProperty (ids::id, newId(), nullptr);
    clip.setProperty (ids::generatorId, generator.getId(), nullptr);
    clip.setProperty (ids::start, std::max (0.0, startBeats), nullptr);
    clip.setProperty (ids::length, std::max (AudioClip::minLengthSeconds, lengthSeconds), nullptr);
    state.appendChild (clip, um);

    AudioClip wrapper (clip);
    wrapper.setFile (file, um);
    return wrapper;
}

void Playlist::removeAudioClip (const AudioClip& clip, juce::UndoManager* um)
{
    state.removeChild (clip.state, um);
}

//==============================================================================
// Song

Song Song::create (const juce::String& name)
{
    juce::ValueTree tree (ids::SONG);
    tree.setProperty (ids::name, name, nullptr);
    tree.setProperty (ids::tempo, 120.0, nullptr);
    tree.appendChild (juce::ValueTree (ids::GENERATORS), nullptr);
    tree.appendChild (juce::ValueTree (ids::PLAYLIST), nullptr);

    Song song (tree);

    // A limiter on the master, because the alternative is finding out that a
    // mix clipped by hearing the crackle in the render and going looking for
    // it in the parts. It is an ordinary insert -- visible in the mixer's
    // master strip, adjustable, removable -- rather than something welded to
    // the output, and at its defaults it does nothing at all to a mix that
    // never reaches the ceiling. Only new songs get one: adding it to a song
    // that was mixed without it would change how that song sounds.
    song.getMasterBus().addEffect (limiterEffectType, nullptr, nullptr);

    return song;
}

std::optional<Song> Song::fromXml (const juce::String& xml)
{
    auto tree = juce::ValueTree::fromXml (xml);
    if (! tree.isValid() || ! tree.hasType (ids::SONG))
        return std::nullopt;

    Song song (tree);
    song.ensureClipIds();
    song.dropPatternSlots();
    song.dropDuplicateChanges();
    song.clampValues();
    return song;
}

std::optional<Song> Song::loadFromFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return std::nullopt;

    auto song = fromXml (file.loadFileAsString());

    // Not in fromXml: this is the only place that knows where the song lives,
    // which is exactly what a stored relative path is relative to.
    if (song)
        song->resolveMediaPaths (file);

    return song;
}

juce::String Song::toXmlString() const
{
    return state.toXmlString();
}

bool Song::saveToFile (const juce::File& file) const
{
    refreshMediaPaths (file);
    return file.replaceWithText (toXmlString());
}

// Both of these write to the tree from a const method, the way getPlaylist
// already does: the wrapper is a handle onto shared state, and neither is an
// edit the user should be able to undo.
void Song::ensureClipIds() const
{
    // By value: the wrapper is a handle onto shared state, and writing needs a
    // non-const one -- the same shape resolveMediaPaths uses below.
    for (auto clip : getPlaylist().getClips())
        if (clip.getId().isEmpty())
            clip.state.setProperty (ids::id, newId(), nullptr);

    for (auto clip : getPlaylist().getAudioClips())
        if (clip.getId().isEmpty())
            clip.state.setProperty (ids::id, newId(), nullptr);
}

void Song::dropPatternSlots() const
{
    // "slot" is not a declared id any more, so it is named here as the string
    // it is in the file. Nothing else in the app knows the property exists.
    static const juce::Identifier legacySlot { "slot" };

    for (auto generator : getGenerators())
        for (auto pattern : generator.getPatterns())
            pattern.state.removeProperty (legacySlot, nullptr);
}

void Song::dropDuplicateChanges() const
{
    // Walked backwards and kept last-wins, which is the one the walks in
    // getTempoAt and secondsFromBeats had in force: both write over what they
    // have for every change at or before the beat, so of a pair on one beat
    // the later one is the one that was heard. This therefore drops only what
    // nothing could hear, and a load leaves the time axis where it was.
    auto dropDuplicatesIn = [] (juce::ValueTree parent, const juce::Identifier& type)
    {
        if (! parent.isValid())
            return;

        std::vector<double> kept;

        for (int i = parent.getNumChildren(); --i >= 0;)
        {
            const auto child = parent.getChild (i);

            if (! child.hasType (type))
                continue;

            const auto beat = (double) child.getProperty (ids::start);

            if (std::any_of (kept.begin(), kept.end(),
                             [beat] (double other) { return isSameBeat (other, beat); }))
                parent.removeChild (i, nullptr);
            else
                kept.push_back (beat);
        }
    };

    dropDuplicatesIn (state.getChildWithName (ids::TEMPOS), ids::TEMPO);
    dropDuplicatesIn (state.getChildWithName (ids::TIMESIGS), ids::TIMESIG);
}

void Song::clampValues() const
{
    // Written straight onto the tree with a null UndoManager, like the two
    // passes above: what a load fixes up is not an edit the user made, and not
    // one they should be able to undo.
    auto clamp = [] (juce::ValueTree node, const juce::Identifier& property, auto clamped)
    {
        if (node.hasProperty (property))
            node.setProperty (property, clamped (node.getProperty (property)), nullptr);
    };

    for (const auto& generator : getGenerators())
        for (const auto& pattern : generator.getPatterns())
        {
            clamp (pattern.state, ids::lengthBeats,
                   [] (const juce::var& v) { return Pattern::clampLengthBeats (v); });

            for (const auto& note : pattern.getNotes())
            {
                clamp (note.state, ids::start,    [] (const juce::var& v) { return Note::clampStart (v); });
                clamp (note.state, ids::length,   [] (const juce::var& v) { return Note::clampLength (v); });
                clamp (note.state, ids::pitch,    [] (const juce::var& v) { return Note::clampPitch (v); });
                clamp (note.state, ids::velocity, [] (const juce::var& v) { return Note::clampVelocity (v); });
            }
        }

    auto playlist = getPlaylist();

    for (const auto& clip : playlist.getClips())
    {
        clamp (clip.state, ids::start,  [] (const juce::var& v) { return PlaylistClip::clampStart (v); });
        clamp (clip.state, ids::length, [] (const juce::var& v) { return PlaylistClip::clampLength (v); });
    }

    for (const auto& clip : playlist.getAudioClips())
    {
        clamp (clip.state, ids::start,  [] (const juce::var& v) { return std::max (0.0, (double) v); });
        clamp (clip.state, ids::offset, [] (const juce::var& v) { return std::max (0.0, (double) v); });
        clamp (clip.state, ids::length,
               [] (const juce::var& v) { return std::max (AudioClip::minLengthSeconds, (double) v); });
    }

    clamp (state, ids::tempo, [] (const juce::var& v) { return TempoChange::clampBpm (v); });

    // Through the setter, because the loop range is a pair: clamping the two
    // ends one at a time could leave them crossed over.
    if (state.hasProperty (ids::loopStart) || state.hasProperty (ids::loopEnd))
        Song (state).setLoopRange (getLoopStart(), getLoopEnd(), nullptr);
}

void Song::resolveMediaPaths (const juce::File& songFile) const
{
    const auto directory = songFile.getParentDirectory();

    for (const auto& generator : getGenerators())
        for (const auto& sound : generator.getSounds())
            FileRef::resolve (sound.state, directory);

    for (const auto& clip : getPlaylist().getAudioClips())
        FileRef::resolve (clip.state, directory);
}

void Song::refreshMediaPaths (const juce::File& songFile) const
{
    const auto directory = songFile.getParentDirectory();

    for (const auto& generator : getGenerators())
        for (const auto& sound : generator.getSounds())
            FileRef::refresh (sound.state, directory);

    for (const auto& clip : getPlaylist().getAudioClips())
        FileRef::refresh (clip.state, directory);
}

std::vector<juce::ValueTree> Song::findMissingMedia() const
{
    std::vector<juce::ValueTree> missing;

    auto consider = [&missing] (const juce::ValueTree& node)
    {
        if (FileRef::hasFile (node) && ! FileRef::getFile (node).existsAsFile())
            missing.push_back (node);
    };

    for (const auto& generator : getGenerators())
        for (const auto& sound : generator.getSounds())
            consider (sound.state);

    for (const auto& clip : getPlaylist().getAudioClips())
        consider (clip.state);

    return missing;
}

int Song::relinkMissingMedia (const juce::File& folder, juce::UndoManager* um) const
{
    const auto missing = findMissingMedia();

    if (missing.empty() || ! folder.isDirectory())
        return 0;

    // One walk of the folder for every name rather than one walk per name:
    // a sample library is large, and a song can be missing dozens of files.
    std::map<juce::String, juce::File> found;
    size_t stillToFind = 0;

    for (const auto& node : missing)
        if (found.emplace (FileRef::getFileName (node), juce::File()).second)
            ++stillToFind;

    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
    {
        if (stillToFind == 0)
            break;

        const auto file = entry.getFile();
        auto it = found.find (file.getFileName());

        if (it != found.end() && it->second == juce::File())
        {
            it->second = file;
            --stillToFind;
        }
    }

    int relinked = 0;

    // By value: the wrapper is a handle onto shared state, and setFile needs
    // a non-const tree.
    for (auto node : missing)
    {
        const auto it = found.find (FileRef::getFileName (node));

        if (it == found.end() || it->second == juce::File())
            continue;

        FileRef::setFile (node, it->second, um);
        ++relinked;
    }

    return relinked;
}

void Song::setLoopRange (double startBeats, double endBeats, juce::UndoManager* um)
{
    // Normalise so a right-to-left drag on the ruler still gives a valid range.
    // Both ends are clamped, not just the start: clamping one of a pair that
    // both sit before the song would leave the range inverted.
    const auto start = std::max (0.0, std::min (startBeats, endBeats));
    const auto end = std::max (0.0, std::max (startBeats, endBeats));

    state.setProperty (ids::loopStart, start, um);
    state.setProperty (ids::loopEnd, end, um);
}

void Song::clearLoopRange (juce::UndoManager* um)
{
    state.removeProperty (ids::loopStart, um);
    state.removeProperty (ids::loopEnd, um);
}

int Song::getNumGenerators() const
{
    return state.getChildWithName (ids::GENERATORS).getNumChildren();
}

Generator Song::getGenerator (int index) const
{
    return Generator (state.getChildWithName (ids::GENERATORS).getChild (index));
}

std::vector<Generator> Song::getGenerators() const
{
    return collectChildren<Generator> (state.getChildWithName (ids::GENERATORS), ids::GENERATOR);
}

std::optional<Generator> Song::findGenerator (const juce::String& generatorId) const
{
    auto found = state.getChildWithName (ids::GENERATORS)
                      .getChildWithProperty (ids::id, generatorId);
    if (! found.isValid())
        return std::nullopt;
    return Generator (found);
}

Generator Song::addGenerator (const juce::String& name, const juce::String& type, juce::UndoManager* um)
{
    juce::ValueTree generator (ids::GENERATOR);
    generator.setProperty (ids::id, newId(), nullptr);
    generator.setProperty (ids::name, name, nullptr);
    generator.setProperty (ids::type, type, nullptr);

    // An audio generator plays files off the playlist, so it never has
    // patterns; giving it an empty PATTERNS node would only invite a view to
    // offer a pattern grid for a row that can't play one.
    if (type != Generator::audioType)
        generator.appendChild (juce::ValueTree (ids::PATTERNS), nullptr);

    getOrCreateChild (state, ids::GENERATORS).appendChild (generator, um);
    return Generator (generator);
}

void Song::removeGenerator (const Generator& generator, juce::UndoManager* um)
{
    const auto id = generator.getId();
    auto playlist = getPlaylist();

    for (const auto& clip : playlist.getClips())
        if (clip.getGeneratorId() == id)
            playlist.removeClip (clip, um);

    for (const auto& clip : playlist.getAudioClips())
        if (clip.getGeneratorId() == id)
            playlist.removeAudioClip (clip, um);

    // A compressor keyed from this generator would keep naming it; the sync
    // rebuilds sidechains from these ids, so a stale one is a broken bus.
    auto clearSidechains = [&id, um] (std::vector<Effect> effects)
    {
        for (auto effect : effects)
            if (effect.getSidechainSourceId() == id)
                effect.setSidechainSourceId ({}, um);
    };

    for (const auto& other : getGenerators())
        clearSidechains (other.getEffects());

    for (const auto& ret : getReturns())
        clearSidechains (ret.getEffects());

    state.getChildWithName (ids::GENERATORS).removeChild (generator.state, um);
}

Playlist Song::getPlaylist() const
{
    return Playlist (getOrCreateChild (state, ids::PLAYLIST));
}

double Song::getLengthBeats() const
{
    double length = 0.0;
    auto playlist = getPlaylist();

    for (const auto& clip : playlist.getClips())
        if (auto generator = findGenerator (clip.getGeneratorId()))
            if (auto pattern = generator->findPattern (clip.getPatternId()))
                length = std::max (length, clip.getStart() + clip.getLength (pattern->getLengthBeats()));

    // An audio clip's length is in seconds, so where it ends in beats depends
    // on the tempo it plays through.
    for (const auto& clip : playlist.getAudioClips())
        length = std::max (length,
                           beatsFromSeconds (secondsFromBeats (clip.getStart())
                                                 + clip.getLengthSeconds()));

    return length;
}

bool Song::isPatternUsedInPlaylist (const Pattern& pattern) const
{
    const auto patternId = pattern.getId();

    for (const auto& clip : getPlaylist().getClips())
        if (clip.getPatternId() == patternId)
            return true;

    return false;
}



//==============================================================================
// MasterBus
//
// The effect list is the generator's, verbatim: same node type, same ids, same
// order semantics. Kept as its own set of methods rather than a shared base
// because a Generator is not a MasterBus in any other respect.

std::vector<Effect> MasterBus::getEffects() const
{
    return collectChildren<Effect> (state.getChildWithName (ids::EFFECTS), ids::EFFECT);
}

std::optional<Effect> MasterBus::findEffect (const juce::String& effectId) const
{
    auto found = state.getChildWithName (ids::EFFECTS).getChildWithProperty (ids::id, effectId);
    return found.isValid() ? std::optional<Effect> (Effect (found)) : std::nullopt;
}

Effect MasterBus::addEffect (const juce::String& type,
                             const juce::PluginDescription* description,
                             juce::UndoManager* um)
{
    juce::ValueTree effect (ids::EFFECT);
    effect.setProperty (ids::id, newId(), nullptr);
    effect.setProperty (ids::type, type, nullptr);

    getOrCreateChild (state, ids::EFFECTS).appendChild (effect, um);

    Effect wrapper (effect);
    if (description != nullptr)
        wrapper.setPlugin (*description, um);

    return wrapper;
}

void MasterBus::removeEffect (const Effect& effect, juce::UndoManager* um)
{
    state.getChildWithName (ids::EFFECTS).removeChild (effect.state, um);
}

void MasterBus::moveEffect (const Effect& effect, int newIndex, juce::UndoManager* um)
{
    auto effects = state.getChildWithName (ids::EFFECTS);
    const auto from = effects.indexOf (effect.state);

    if (from < 0)
        return;

    effects.moveChild (from, juce::jlimit (0, effects.getNumChildren() - 1, newIndex), um);
}

MasterBus Song::getMasterBus() const
{
    return MasterBus (getOrCreateChild (state, ids::MASTER));
}




//==============================================================================
// Modifiers

std::vector<ModifierAssign> GenModifier::getAssigns() const
{
    return collectChildren<ModifierAssign> (state, ids::ASSIGN);
}

ModifierAssign GenModifier::addAssign (const juce::String& target, const juce::String& param,
                                       juce::UndoManager* um)
{
    for (const auto& existing : getAssigns())
        if (existing.getTarget() == target && existing.getParam() == param)
            return existing;

    juce::ValueTree assign (ids::ASSIGN);
    assign.setProperty (ids::target, target, nullptr);

    if (param.isNotEmpty())
        assign.setProperty (ids::param, param, nullptr);

    state.appendChild (assign, um);
    return ModifierAssign (assign);
}

void GenModifier::removeAssign (const ModifierAssign& assign, juce::UndoManager* um)
{
    state.removeChild (assign.state, um);
}

std::vector<GenModifier> Generator::getModifiers() const
{
    return collectChildren<GenModifier> (state.getChildWithName (ids::MODIFIERS), ids::MODIFIER);
}

std::optional<GenModifier> Generator::findModifier (const juce::String& modifierId) const
{
    auto found = state.getChildWithName (ids::MODIFIERS).getChildWithProperty (ids::id, modifierId);
    return found.isValid() ? std::optional<GenModifier> (GenModifier (found)) : std::nullopt;
}

GenModifier Generator::addModifier (const juce::String& kind, juce::UndoManager* um)
{
    juce::ValueTree modifier (ids::MODIFIER);
    modifier.setProperty (ids::id, newId(), nullptr);
    modifier.setProperty (ids::kind, kind, nullptr);
    getOrCreateChild (state, ids::MODIFIERS).appendChild (modifier, um);
    return GenModifier (modifier);
}

void Generator::removeModifier (const GenModifier& modifier, juce::UndoManager* um)
{
    state.getChildWithName (ids::MODIFIERS).removeChild (modifier.state, um);
}

//==============================================================================
// Automation

int AutomationLane::getNumPoints() const
{
    return countChildren (state, ids::PT);
}

std::vector<AutomationPoint> AutomationLane::getPoints() const
{
    auto result = collectChildren<AutomationPoint> (state, ids::PT);

    // Beat order is what every consumer assumes; the tree keeps insertion order.
    std::sort (result.begin(), result.end(),
               [] (const AutomationPoint& a, const AutomationPoint& b) { return a.getBeat() < b.getBeat(); });

    return result;
}

AutomationPoint AutomationLane::addPoint (double beat, float value, juce::UndoManager* um)
{
    juce::ValueTree point (ids::PT);
    state.appendChild (point, um);

    AutomationPoint wrapper (point);
    wrapper.setBeat (beat, um);
    wrapper.setValue (value, um);
    return wrapper;
}

void AutomationLane::removePoint (const AutomationPoint& point, juce::UndoManager* um)
{
    state.removeChild (point.state, um);
}

std::vector<AutomationLane> Generator::getAutomationLanes() const
{
    return collectChildren<AutomationLane> (state.getChildWithName (ids::AUTOMATION), ids::AUTOCURVE);
}

std::optional<AutomationLane> Generator::findAutomationLane (const juce::String& target,
                                                             const juce::String& param) const
{
    for (const auto& lane : getAutomationLanes())
        if (lane.getTarget() == target && lane.getParam() == param)
            return lane;

    return std::nullopt;
}

AutomationLane Generator::addAutomationLane (const juce::String& target,
                                             const juce::String& param, juce::UndoManager* um)
{
    if (auto existing = findAutomationLane (target, param))
        return *existing;

    juce::ValueTree lane (ids::AUTOCURVE);
    lane.setProperty (ids::target, target, nullptr);

    if (param.isNotEmpty())
        lane.setProperty (ids::param, param, nullptr);

    getOrCreateChild (state, ids::AUTOMATION).appendChild (lane, um);
    return AutomationLane (lane);
}

void Generator::removeAutomationLane (const AutomationLane& lane, juce::UndoManager* um)
{
    state.getChildWithName (ids::AUTOMATION).removeChild (lane.state, um);
}

//==============================================================================
// Sends and returns

std::vector<Send> Generator::getSends() const
{
    return collectChildren<Send> (state.getChildWithName (ids::SENDS), ids::SEND);
}

std::optional<Send> Generator::findSend (const juce::String& returnId) const
{
    auto found = state.getChildWithName (ids::SENDS)
                      .getChildWithProperty (ids::returnId, returnId);
    return found.isValid() ? std::optional<Send> (Send (found)) : std::nullopt;
}

Send Generator::setSendGain (const juce::String& returnId, float gainDb, juce::UndoManager* um)
{
    if (auto existing = findSend (returnId))
    {
        existing->setGainDb (gainDb, um);
        return *existing;
    }

    juce::ValueTree send (ids::SEND);
    send.setProperty (ids::returnId, returnId, nullptr);
    getOrCreateChild (state, ids::SENDS).appendChild (send, um);

    Send wrapper (send);
    wrapper.setGainDb (gainDb, um);
    return wrapper;
}

void Generator::removeSend (const juce::String& returnId, juce::UndoManager* um)
{
    if (auto existing = findSend (returnId))
        state.getChildWithName (ids::SENDS).removeChild (existing->state, um);
}

// The effect list is the generator's / master's, verbatim -- one node type,
// one EditSync reconciliation.

std::vector<Effect> Return::getEffects() const
{
    return collectChildren<Effect> (state.getChildWithName (ids::EFFECTS), ids::EFFECT);
}

std::optional<Effect> Return::findEffect (const juce::String& effectId) const
{
    auto found = state.getChildWithName (ids::EFFECTS).getChildWithProperty (ids::id, effectId);
    return found.isValid() ? std::optional<Effect> (Effect (found)) : std::nullopt;
}

Effect Return::addEffect (const juce::String& type,
                          const juce::PluginDescription* description,
                          juce::UndoManager* um)
{
    juce::ValueTree effect (ids::EFFECT);
    effect.setProperty (ids::id, newId(), nullptr);
    effect.setProperty (ids::type, type, nullptr);

    getOrCreateChild (state, ids::EFFECTS).appendChild (effect, um);

    Effect wrapper (effect);
    if (description != nullptr)
        wrapper.setPlugin (*description, um);

    return wrapper;
}

void Return::removeEffect (const Effect& effect, juce::UndoManager* um)
{
    state.getChildWithName (ids::EFFECTS).removeChild (effect.state, um);
}

void Return::moveEffect (const Effect& effect, int newIndex, juce::UndoManager* um)
{
    auto effects = state.getChildWithName (ids::EFFECTS);
    const auto from = effects.indexOf (effect.state);

    if (from < 0)
        return;

    effects.moveChild (from, juce::jlimit (0, effects.getNumChildren() - 1, newIndex), um);
}

//==============================================================================
// Groups. Same effect list again -- see the note above Return's.

std::vector<Effect> Group::getEffects() const
{
    return collectChildren<Effect> (state.getChildWithName (ids::EFFECTS), ids::EFFECT);
}

std::optional<Effect> Group::findEffect (const juce::String& effectId) const
{
    auto found = state.getChildWithName (ids::EFFECTS).getChildWithProperty (ids::id, effectId);
    return found.isValid() ? std::optional<Effect> (Effect (found)) : std::nullopt;
}

Effect Group::addEffect (const juce::String& type,
                         const juce::PluginDescription* description,
                         juce::UndoManager* um)
{
    juce::ValueTree effect (ids::EFFECT);
    effect.setProperty (ids::id, newId(), nullptr);
    effect.setProperty (ids::type, type, nullptr);

    getOrCreateChild (state, ids::EFFECTS).appendChild (effect, um);

    Effect wrapper (effect);
    if (description != nullptr)
        wrapper.setPlugin (*description, um);

    return wrapper;
}

void Group::removeEffect (const Effect& effect, juce::UndoManager* um)
{
    state.getChildWithName (ids::EFFECTS).removeChild (effect.state, um);
}

void Group::moveEffect (const Effect& effect, int newIndex, juce::UndoManager* um)
{
    auto effects = state.getChildWithName (ids::EFFECTS);
    const auto from = effects.indexOf (effect.state);

    if (from < 0)
        return;

    effects.moveChild (from, juce::jlimit (0, effects.getNumChildren() - 1, newIndex), um);
}

std::vector<Group> Song::getGroups() const
{
    return collectChildren<Group> (state.getChildWithName (ids::GROUPS), ids::GROUP);
}

std::optional<Group> Song::findGroup (const juce::String& groupId) const
{
    auto found = state.getChildWithName (ids::GROUPS).getChildWithProperty (ids::id, groupId);
    return found.isValid() ? std::optional<Group> (Group (found)) : std::nullopt;
}

Group Song::addGroup (const juce::String& name, juce::UndoManager* um)
{
    juce::ValueTree group (ids::GROUP);
    group.setProperty (ids::id, newId(), nullptr);
    group.setProperty (ids::name, name, nullptr);
    getOrCreateChild (state, ids::GROUPS).appendChild (group, um);
    return Group (group);
}

void Song::removeGroup (const Group& group, juce::UndoManager* um)
{
    // Members first and in the same transaction: a generator left naming a
    // group that is gone would be routed to nothing, and one press of undo
    // has to bring both halves back together.
    const auto groupId = group.getId();

    for (auto generator : getGenerators())
        if (generator.getGroupId() == groupId)
            generator.setGroupId ({}, um);

    state.getChildWithName (ids::GROUPS).removeChild (group.state, um);
}

std::vector<Generator> Song::getGeneratorsInGroup (const juce::String& groupId) const
{
    std::vector<Generator> members;

    if (groupId.isEmpty())
        return members;

    for (const auto& generator : getGenerators())
        if (generator.getGroupId() == groupId)
            members.push_back (generator);

    return members;
}

std::vector<Return> Song::getReturns() const
{
    return collectChildren<Return> (state.getChildWithName (ids::RETURNS), ids::RETURN);
}

std::optional<Return> Song::findReturn (const juce::String& returnId) const
{
    auto found = state.getChildWithName (ids::RETURNS).getChildWithProperty (ids::id, returnId);
    return found.isValid() ? std::optional<Return> (Return (found)) : std::nullopt;
}

Return Song::addReturn (const juce::String& name, juce::UndoManager* um)
{
    // The lowest unused positive bus number, persisted: AuxSend and AuxReturn
    // find each other by it, so it must not shuffle between loads.
    int bus = 1;

    for (bool taken = true; taken; )
    {
        taken = false;

        for (const auto& existing : getReturns())
            if (existing.getBusNumber() == bus)
            {
                taken = true;
                ++bus;
                break;
            }
    }

    juce::ValueTree ret (ids::RETURN);
    ret.setProperty (ids::id, newId(), nullptr);
    ret.setProperty (ids::name, name, nullptr);
    ret.setProperty (ids::busNumber, bus, nullptr);
    getOrCreateChild (state, ids::RETURNS).appendChild (ret, um);
    return Return (ret);
}

void Song::removeReturn (const Return& ret, juce::UndoManager* um)
{
    // In the same transaction, so one press of undo brings the return and
    // everything that fed it back together.
    const auto returnId = ret.getId();

    for (auto generator : getGenerators())
        generator.removeSend (returnId, um);

    state.getChildWithName (ids::RETURNS).removeChild (ret.state, um);
}

//==============================================================================
// Tempo and time signature

namespace
{
    template <typename Change>
    std::vector<Change> collectSorted (const juce::ValueTree& parent, const juce::Identifier& type)
    {
        auto result = collectChildren<Change> (parent, type);

        // Beat order is what every walk below assumes; the tree keeps whatever
        // order things were added in.
        //
        // Stable, so that if two changes ever do share a beat -- which nothing
        // this code writes can produce, but a tree assembled in memory could,
        // before dropDuplicateChanges gets to it -- document order decides
        // which is in force, rather than whichever the sort happened to leave
        // last. That is what makes "the load keeps the one that was in force"
        // mean anything. It costs a temporary buffer per call, which these
        // lists are far too short for anyone to notice.
        std::stable_sort (result.begin(), result.end(),
                          [] (const Change& a, const Change& b)
                          { return a.getStartBeat() < b.getStartBeat(); });

        return result;
    }

    // Whether a sibling of `change` already sits on `beat`. What stops a drag
    // from stacking one change on another; the two setStartBeat methods share
    // it because a tempo change and a signature change live in their own
    // lists, so a beat is only occupied by a change of the same kind.
    bool beatTakenBySibling (const juce::ValueTree& change, double beat)
    {
        const auto parent = change.getParent();

        if (! parent.isValid())
            return false;

        for (const auto& sibling : parent)
            if (sibling != change && sibling.hasType (change.getType())
                 && isSameBeat ((double) sibling.getProperty (ids::start), beat))
                return true;

        return false;
    }

    // The shared body of TempoChange::setStartBeat and TimeSigChange's.
    void moveChangeTo (juce::ValueTree change, double beat, juce::UndoManager* um)
    {
        const auto target = juce::jmax (0.0, beat);

        if (! beatTakenBySibling (change, target))
            change.setProperty (ids::start, target, um);
    }

    // The change of this kind sitting on this beat. Walks the children rather
    // than collectSorted's vector: this is a point query, order means nothing
    // to it, and it runs on the marker drag path once per mouse move.
    template <typename Change>
    std::optional<Change> findChangeAt (const juce::ValueTree& parent,
                                        const juce::Identifier& type, double beat)
    {
        for (const auto& child : parent)
            if (child.hasType (type) && isSameBeat ((double) child.getProperty (ids::start), beat))
                return Change (child);

        return std::nullopt;
    }

    // A change node, built before it is appended -- the way every other add in
    // this file works: one undoable action instead of three, and the node is
    // never briefly in the list without the beat it belongs on. The payload is
    // whatever the kind of change carries.
    template <typename Change, typename WritePayload>
    Change appendChange (juce::ValueTree parent, const juce::Identifier& type,
                         double beat, WritePayload&& writePayload, juce::UndoManager* um)
    {
        juce::ValueTree change (type);
        change.setProperty (ids::start, beat, nullptr);
        writePayload (change);
        parent.appendChild (change, um);
        return Change (change);
    }
} // namespace

void TempoChange::setStartBeat (double beat, juce::UndoManager* um)
{
    moveChangeTo (state, beat, um);
}

void TimeSigChange::setStartBeat (double beat, juce::UndoManager* um)
{
    moveChangeTo (state, beat, um);
}

std::vector<TempoChange> Song::getTempoChanges() const
{
    return collectSorted<TempoChange> (state.getChildWithName (ids::TEMPOS), ids::TEMPO);
}

std::optional<TempoChange> Song::findTempoChangeAt (double beat) const
{
    return findChangeAt<TempoChange> (state.getChildWithName (ids::TEMPOS), ids::TEMPO, beat);
}

TempoChange Song::addTempoChange (double startBeat, double bpm, juce::UndoManager* um)
{
    // Clamped before the lookup, not after: setStartBeat clamps too, so a beat
    // before the song would otherwise be looked up where it was asked for and
    // written where it lands.
    const auto beat = juce::jmax (0.0, startBeat);

    if (auto existing = findTempoChangeAt (beat))
        return *existing;

    return appendChange<TempoChange> (getOrCreateChild (state, ids::TEMPOS), ids::TEMPO, beat,
                                      [bpm] (juce::ValueTree& change)
                                      {
                                          change.setProperty (ids::bpm, TempoChange::clampBpm (bpm),
                                                              nullptr);
                                      },
                                      um);
}

void Song::removeTempoChange (const TempoChange& change, juce::UndoManager* um)
{
    state.getChildWithName (ids::TEMPOS).removeChild (change.state, um);
}

double Song::getTempoAt (double beat) const
{
    double bpm = getTempo();

    for (const auto& change : getTempoChanges())
    {
        if (change.getStartBeat() > beat)
            break;

        bpm = change.getBpm();
    }

    return bpm;
}

std::vector<TimeSigChange> Song::getTimeSigChanges() const
{
    return collectSorted<TimeSigChange> (state.getChildWithName (ids::TIMESIGS), ids::TIMESIG);
}

std::optional<TimeSigChange> Song::findTimeSigChangeAt (double beat) const
{
    return findChangeAt<TimeSigChange> (state.getChildWithName (ids::TIMESIGS), ids::TIMESIG, beat);
}

TimeSigChange Song::addTimeSigChange (double startBeat, TimeSignature sig, juce::UndoManager* um)
{
    const auto beat = juce::jmax (0.0, startBeat);

    if (auto existing = findTimeSigChangeAt (beat))
        return *existing;

    return appendChange<TimeSigChange> (getOrCreateChild (state, ids::TIMESIGS), ids::TIMESIG, beat,
                                        [sig] (juce::ValueTree& change)
                                        {
                                            change.setProperty (ids::numerator, juce::jmax (1, sig.numerator), nullptr);
                                            change.setProperty (ids::denominator, juce::jmax (1, sig.denominator), nullptr);
                                        },
                                        um);
}

void Song::removeTimeSigChange (const TimeSigChange& change, juce::UndoManager* um)
{
    state.getChildWithName (ids::TIMESIGS).removeChild (change.state, um);
}

TimeSignature Song::getTimeSigAt (double beat) const
{
    TimeSignature sig;

    for (const auto& change : getTimeSigChanges())
    {
        if (change.getStartBeat() > beat)
            break;

        sig = change.getSignature();
    }

    return sig;
}

double Song::secondsFromBeats (double beats) const
{
    if (beats <= 0.0)
        return 0.0;

    // getTempo and getBpm both floor what they return, so nothing below has to
    // guard against dividing by zero.
    double seconds = 0.0, cursor = 0.0, bpm = getTempo();

    for (const auto& change : getTempoChanges())
    {
        const auto at = change.getStartBeat();

        if (at >= beats)
            break;

        if (at > cursor)
        {
            seconds += (at - cursor) * 60.0 / bpm;
            cursor = at;
        }

        bpm = change.getBpm();
    }

    return seconds + (beats - cursor) * 60.0 / bpm;
}

double Song::beatsFromSeconds (double seconds) const
{
    if (seconds <= 0.0)
        return 0.0;

    double elapsed = 0.0, cursor = 0.0, bpm = getTempo();

    for (const auto& change : getTempoChanges())
    {
        const auto at = change.getStartBeat();

        if (at > cursor)
        {
            const auto span = (at - cursor) * 60.0 / bpm;

            // The answer falls inside this stretch of constant tempo.
            if (elapsed + span >= seconds)
                return cursor + (seconds - elapsed) * bpm / 60.0;

            elapsed += span;
            cursor = at;
        }

        bpm = change.getBpm();
    }

    return cursor + (seconds - elapsed) * bpm / 60.0;
}

Song::BarsAndBeats Song::toBarsAndBeats (double beat) const
{
    // A tolerance, because a bar line computed by division lands a hair either
    // side of the beat a clip was snapped to.
    constexpr double tolerance = 1.0e-9;

    int bar = 0;
    double cursor = 0.0;
    TimeSignature sig;

    for (const auto& change : getTimeSigChanges())
    {
        const auto at = change.getStartBeat();

        if (at > beat)
            break;

        if (at > cursor)
        {
            // ceil, not floor: a change that lands mid-bar cuts that bar short
            // rather than being swallowed by it, which is what every DAW does
            // and what keeps this agreeing with beatOfBar.
            bar += (int) std::ceil ((at - cursor) / sig.getBeatsPerBar() - tolerance);
            cursor = at;
        }

        sig = change.getSignature();
    }

    const auto into = beat - cursor;
    const auto barsIn = std::floor (into / sig.getBeatsPerBar() + tolerance);

    return { bar + (int) barsIn, into - barsIn * sig.getBeatsPerBar() };
}

double Song::beatOfBar (int bar) const
{
    constexpr double tolerance = 1.0e-9;

    int barCursor = 0;
    double cursor = 0.0;
    TimeSignature sig;

    for (const auto& change : getTimeSigChanges())
    {
        const auto at = change.getStartBeat();

        // Matches toBarsAndBeats: a short bar before the change still counts.
        const auto barsUntil = (int) std::ceil ((at - cursor) / sig.getBeatsPerBar() - tolerance);

        if (barCursor + barsUntil > bar)
            break;

        barCursor += barsUntil;
        cursor = at;
        sig = change.getSignature();
    }

    return cursor + (bar - barCursor) * sig.getBeatsPerBar();
}

} // namespace carve::model
