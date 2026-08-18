#include "SongModel.h"

namespace orionish::model
{

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

Pattern Generator::getOrCreatePatternInSlot (const PatternSlot& slot, juce::UndoManager* um)
{
    if (auto existing = findPatternInSlot (slot))
        return *existing;

    // The slot key doubles as the default name, so an untouched slot reads as
    // "A1" everywhere until the user renames it.
    return addPattern (slot, slot.getKey(), Pattern::defaultLengthBeats, um);
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
    return Song (tree);
}

std::optional<Song> Song::loadFromFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return std::nullopt;
    return fromXml (file.loadFileAsString());
}

juce::String Song::toXmlString() const
{
    return state.toXmlString();
}

bool Song::saveToFile (const juce::File& file) const
{
    return file.replaceWithText (toXmlString());
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
    generator.appendChild (juce::ValueTree (ids::PATTERNS), nullptr);
    getOrCreateChild (state, ids::GENERATORS).appendChild (generator, um);
    return Generator (generator);
}

Playlist Song::getPlaylist() const
{
    return Playlist (getOrCreateChild (state, ids::PLAYLIST));
}

bool Song::isPatternUsedInPlaylist (const Pattern& pattern) const
{
    const auto patternId = pattern.getId();

    for (const auto& clip : getPlaylist().getClips())
        if (clip.getPatternId() == patternId)
            return true;

    return false;
}

} // namespace orionish::model
