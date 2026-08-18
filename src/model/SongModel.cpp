#include "SongModel.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace carve::model
{

// tracktion names a plugin's state tree "PLUGIN"; spelled out here so the model
// layer doesn't have to include the engine just for one identifier.
static const juce::Identifier te_pluginStateType ("PLUGIN");

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
} // namespace

//==============================================================================
// PatternSlot

std::optional<PatternSlot> PatternSlot::fromKey (const juce::String& key)
{
    if (key.length() < 2)
        return std::nullopt;

    const PatternSlot slot { key[0] - 'A', key.substring (1).getIntValue() - 1 };
    if (! slot.isValid() || slot.getKey() != key)   // rejects "A01", "AA1", ...
        return std::nullopt;

    return slot;
}

//==============================================================================
// Pattern

bool Pattern::hasDefaultSlotName() const
{
    if (auto slot = getSlot())
        return getName() == slot->getKey();

    return false;
}

int Pattern::getNumNotes() const
{
    return state.getNumChildren();
}

Note Pattern::getNote (int index) const
{
    return Note (state.getChild (index));
}

std::vector<Note> Pattern::getNotes() const
{
    return collectChildren<Note> (state, ids::NOTE);
}

Note Pattern::addNote (double startBeats, double lengthBeats, int pitch, int velocity, juce::UndoManager* um)
{
    juce::ValueTree note (ids::NOTE);
    note.setProperty (ids::start, startBeats, nullptr);
    note.setProperty (ids::length, lengthBeats, nullptr);
    note.setProperty (ids::pitch, pitch, nullptr);
    note.setProperty (ids::velocity, velocity, nullptr);
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

std::vector<Pattern> Generator::getUnslottedPatterns() const
{
    std::vector<Pattern> result;
    for (const auto& pattern : getPatterns())
        if (! pattern.getSlot())
            result.push_back (pattern);
    return result;
}

std::optional<Pattern> Generator::findPatternInSlot (const PatternSlot& slot) const
{
    if (! slot.isValid())
        return std::nullopt;

    auto found = state.getChildWithName (ids::PATTERNS)
                      .getChildWithProperty (ids::slot, slot.getKey());
    if (! found.isValid())
        return std::nullopt;

    return Pattern (found);
}

std::optional<PatternSlot> Generator::findFreeSlot (std::optional<PatternSlot> after) const
{
    const int start = after && after->isValid() ? after->toFlatIndex() + 1 : 0;

    for (int i = 0; i < PatternSlot::numSlots; ++i)
    {
        const auto slot = PatternSlot::fromFlatIndex ((start + i) % PatternSlot::numSlots);
        if (! findPatternInSlot (slot))
            return slot;
    }

    return std::nullopt;
}

Pattern Generator::duplicatePattern (const Pattern& source, const PatternSlot& destination,
                                     juce::UndoManager* um)
{
    // A pattern still carrying its slot's name was never named by the user, so
    // the copy takes its own slot's name and the grid stays readable. One the
    // user did name keeps that name, marked as the copy it is.
    const auto name = source.hasDefaultSlotName() ? destination.getKey()
                                                  : source.getName() + " copy";

    auto copy = addPattern (destination, name, source.getLengthBeats(), um);
    copy.copyNotesFrom (source, um);
    return copy;
}

Pattern Generator::getOrCreatePatternInSlot (const PatternSlot& slot, juce::UndoManager* um,
                                             double lengthBeats)
{
    if (auto existing = findPatternInSlot (slot))
        return *existing;

    // The slot key doubles as the default name, so an untouched slot reads as
    // "A1" everywhere until the user renames it.
    return addPattern (slot, slot.getKey(), lengthBeats, um);
}

Pattern Generator::addPattern (const juce::String& name, double lengthBeats, juce::UndoManager* um)
{
    juce::ValueTree pattern (ids::PATTERN);
    pattern.setProperty (ids::id, newId(), nullptr);
    pattern.setProperty (ids::name, name, nullptr);
    pattern.setProperty (ids::lengthBeats, lengthBeats, nullptr);
    getOrCreateChild (state, ids::PATTERNS).appendChild (pattern, um);
    return Pattern (pattern);
}

Pattern Generator::addPattern (const PatternSlot& slot, const juce::String& name,
                               double lengthBeats, juce::UndoManager* um)
{
    auto pattern = addPattern (name, lengthBeats, um);
    if (slot.isValid())
        pattern.setSlot (slot, um);
    return pattern;
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

//==============================================================================
// Playlist

int Playlist::getNumClips() const
{
    return state.getNumChildren();
}

PlaylistClip Playlist::getClip (int index) const
{
    return PlaylistClip (state.getChild (index));
}

std::vector<PlaylistClip> Playlist::getClips() const
{
    return collectChildren<PlaylistClip> (state, ids::CLIP);
}

PlaylistClip Playlist::addClip (const Generator& generator, const Pattern& pattern,
                                double startBeats, juce::UndoManager* um)
{
    juce::ValueTree clip (ids::CLIP);
    clip.setProperty (ids::generatorId, generator.getId(), nullptr);
    clip.setProperty (ids::patternId, pattern.getId(), nullptr);
    clip.setProperty (ids::start, startBeats, nullptr);
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
    return Song (tree);
}

std::optional<Song> Song::fromXml (const juce::String& xml)
{
    auto tree = juce::ValueTree::fromXml (xml);
    if (! tree.isValid() || ! tree.hasType (ids::SONG))
        return std::nullopt;

    Song song (tree);
    song.ensureAudioClipIds();
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
void Song::ensureAudioClipIds() const
{
    // By value: the wrapper is a handle onto shared state, and writing needs a
    // non-const one -- the same shape resolveMediaPaths uses below.
    for (auto clip : getPlaylist().getAudioClips())
        if (clip.getId().isEmpty())
            clip.state.setProperty (ids::id, newId(), nullptr);
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

void Song::setLoopRange (double startBeats, double endBeats, juce::UndoManager* um)
{
    // Normalise so a right-to-left drag on the ruler still gives a valid range.
    const auto start = std::max (0.0, std::min (startBeats, endBeats));
    const auto end = std::max (startBeats, endBeats);

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
// Tempo and time signature

namespace
{
    template <typename Change>
    std::vector<Change> collectSorted (const juce::ValueTree& parent, const juce::Identifier& type)
    {
        auto result = collectChildren<Change> (parent, type);

        // Beat order is what every walk below assumes; the tree keeps whatever
        // order things were added in.
        std::sort (result.begin(), result.end(),
                   [] (const Change& a, const Change& b) { return a.getStartBeat() < b.getStartBeat(); });

        return result;
    }
} // namespace

std::vector<TempoChange> Song::getTempoChanges() const
{
    return collectSorted<TempoChange> (state.getChildWithName (ids::TEMPOS), ids::TEMPO);
}

TempoChange Song::addTempoChange (double startBeat, double bpm, juce::UndoManager* um)
{
    juce::ValueTree change (ids::TEMPO);
    getOrCreateChild (state, ids::TEMPOS).appendChild (change, um);

    TempoChange wrapper (change);
    wrapper.setStartBeat (startBeat, um);
    wrapper.setBpm (bpm, um);
    return wrapper;
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

    return bpm > 0.0 ? bpm : 120.0;
}

std::vector<TimeSigChange> Song::getTimeSigChanges() const
{
    return collectSorted<TimeSigChange> (state.getChildWithName (ids::TIMESIGS), ids::TIMESIG);
}

TimeSigChange Song::addTimeSigChange (double startBeat, TimeSignature sig, juce::UndoManager* um)
{
    juce::ValueTree change (ids::TIMESIG);
    getOrCreateChild (state, ids::TIMESIGS).appendChild (change, um);

    TimeSigChange wrapper (change);
    wrapper.setStartBeat (startBeat, um);
    wrapper.setSignature (sig, um);
    return wrapper;
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

    double seconds = 0.0, cursor = 0.0, bpm = getTempo() > 0.0 ? getTempo() : 120.0;

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

    double elapsed = 0.0, cursor = 0.0, bpm = getTempo() > 0.0 ? getTempo() : 120.0;

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
