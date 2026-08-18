#include "EditSync.h"

namespace orionish::sync
{

namespace
{
    // Volume/pan round-trip through the fader taper, so compare loosely rather
    // than re-setting (and re-notifying) the parameter on every sync.
    void applyMixerState (const model::Generator& generator, te::AudioTrack& track)
    {
        if (auto volume = track.getVolumePlugin())
        {
            if (std::abs (volume->getVolumeDb() - generator.getVolumeDb()) > 0.01f)
                volume->setVolumeDb (generator.getVolumeDb());

            if (std::abs (volume->getPan() - generator.getPan()) > 0.001f)
                volume->setPan (generator.getPan());
        }

        if (track.isMuted (false) != generator.isMuted())
            track.setMute (generator.isMuted());

        if (track.isSolo (false) != generator.isSoloed())
            track.setSolo (generator.isSoloed());
    }

    void removeInstruments (te::AudioTrack& track)
    {
        for (auto plugin : track.pluginList.getPlugins())
            if (dynamic_cast<te::FourOscPlugin*> (plugin) != nullptr
                 || dynamic_cast<te::ExternalPlugin*> (plugin) != nullptr)
                plugin->deleteFromParent();
    }

    void restorePluginState (te::ExternalPlugin& external, const juce::String& base64)
    {
        if (base64.isEmpty())
            return;

        juce::MemoryBlock block;
        if (! block.fromBase64Encoding (base64) || block.isEmpty())
            return;

        if (auto instance = external.getAudioPluginInstance())
            instance->setStateInformation (block.getData(), (int) block.getSize());
    }

    void ensureExternalInstrument (te::Edit& edit, te::AudioTrack& track,
                                   const model::Generator& generator,
                                   const juce::PluginDescription& description)
    {
        if (auto existing = track.pluginList.findFirstPluginOfType<te::ExternalPlugin>())
            if (te::createIdentifierString (existing->desc)
                    == te::createIdentifierString (description))
                return;

        removeInstruments (track);

        auto plugin = edit.getPluginCache().createNewPlugin (
            te::ExternalPlugin::create (edit.engine, description));
        if (plugin == nullptr)
            return;

        track.pluginList.insertPlugin (plugin, 0, nullptr);

        if (auto external = dynamic_cast<te::ExternalPlugin*> (plugin.get()))
            restorePluginState (*external, generator.getPluginState());
    }

    void ensureInternalInstrument (te::Edit& edit, te::AudioTrack& track)
    {
        if (track.pluginList.findFirstPluginOfType<te::FourOscPlugin>() != nullptr)
            return;

        removeInstruments (track);

        if (auto synth = dynamic_cast<te::FourOscPlugin*> (
                edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}).get()))
            track.pluginList.insertPlugin (*synth, 0, nullptr);
    }

    void ensureInstrument (te::Edit& edit, te::AudioTrack& track, const model::Generator& generator)
    {
        if (generator.getType() == "plugin")
        {
            if (auto description = generator.getPluginDescription())
                ensureExternalInstrument (edit, track, generator, *description);
            return;
        }
        ensureInternalInstrument (edit, track);
    }

    void rebuildClips (const model::Song& song, const model::Generator& generator,
                       te::Edit& edit, te::AudioTrack& track)
    {
        for (auto clip : juce::Array<te::Clip*> (track.getClips()))   // copy: removal mutates the list
            clip->removeFromParent();

        for (const auto& placement : song.getPlaylist().getClips())
        {
            if (placement.getGeneratorId() != generator.getId())
                continue;

            auto pattern = generator.findPattern (placement.getPatternId());
            if (! pattern)
                continue;

            const auto patternLength = pattern->getLengthBeats();
            const auto clipLength = placement.getLength (patternLength);

            if (patternLength <= 0.0 || clipLength <= 0.0)
                continue;

            const auto startBeat = placement.getStart();
            const te::BeatRange beats (te::BeatPosition::fromBeats (startBeat),
                                       te::BeatPosition::fromBeats (startBeat + clipLength));
            auto midiClip = track.insertMIDIClip (pattern->getName(),
                                                  edit.tempoSequence.toTime (beats), nullptr);
            if (midiClip == nullptr)
                continue;

            const auto transpose = placement.getTranspose();
            const auto notes = pattern->getNotes();

            // A placement can be longer than its pattern, in which case the
            // pattern repeats, and shorter, in which case it is cut off. The
            // repeat count is capped so a pattern shortened to almost nothing
            // can't spin here.
            constexpr int maxRepeats = 512;

            for (int repeat = 0; repeat < maxRepeats; ++repeat)
            {
                const auto offset = repeat * patternLength;

                if (offset >= clipLength - 1.0e-9)
                    break;

                for (const auto& note : notes)
                {
                    const auto noteStart = offset + note.getStart();

                    if (noteStart >= clipLength - 1.0e-9)
                        continue;

                    midiClip->getSequence().addNote (
                        juce::jlimit (0, 127, note.getPitch() + transpose),
                        te::BeatPosition::fromBeats (noteStart),
                        te::BeatDuration::fromBeats (std::min (note.getLength(),
                                                               clipLength - noteStart)),
                        note.getVelocity(), 0, nullptr);
                }
            }
        }
    }
} // namespace

void syncSongToEdit (const model::Song& song, te::Edit& edit)
{
    if (auto tempo = edit.tempoSequence.getTempo (0))
        if (tempo->getBpm() != song.getTempo())
            tempo->setBpm (song.getTempo());

    const auto generators = song.getGenerators();

    edit.ensureNumberOfAudioTracks ((int) generators.size());
    auto tracks = te::getAudioTracks (edit);

    while (tracks.size() > (int) generators.size())   // generator was deleted
    {
        edit.deleteTrack (tracks.getLast());
        tracks = te::getAudioTracks (edit);
    }

    for (int i = 0; i < (int) generators.size(); ++i)
    {
        const auto& generator = generators[(size_t) i];
        auto& track = *tracks[i];

        if (track.getName() != generator.getName())
            track.setName (generator.getName());

        ensureInstrument (edit, track, generator);
        applyMixerState (generator, track);
        rebuildClips (song, generator, edit, track);
    }
}

void EditSync::captureLivePluginState()
{
    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
    {
        auto generator = generators[(size_t) i];
        if (generator.getType() != "plugin")
            continue;

        if (auto external = tracks[i]->pluginList.findFirstPluginOfType<te::ExternalPlugin>())
        {
            if (auto instance = external->getAudioPluginInstance())
            {
                juce::MemoryBlock block;
                instance->getStateInformation (block);
                if (! block.isEmpty())
                    generator.setPluginState (block.toBase64Encoding(), nullptr);
            }
        }
    }
}

EditSync::EditSync (model::Song songToWatch, te::Edit& targetEdit)
    : song (std::move (songToWatch)), edit (targetEdit)
{
    song.state.addListener (this);
    resyncNow();
}

EditSync::~EditSync()
{
    cancelPendingUpdate();
    song.state.removeListener (this);
}

void EditSync::resyncNow()
{
    cancelPendingUpdate();
    syncSongToEdit (song, edit);
}

void EditSync::applyMixerStateOnly()
{
    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
        applyMixerState (generators[(size_t) i], *tracks[i]);
}

} // namespace orionish::sync
