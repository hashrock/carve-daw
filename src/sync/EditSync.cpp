#include "EditSync.h"

namespace orionish::sync
{

namespace
{
    void ensureInstrument (te::Edit& edit, te::AudioTrack& track)
    {
        if (track.pluginList.findFirstPluginOfType<te::FourOscPlugin>() != nullptr)
            return;

        if (auto synth = dynamic_cast<te::FourOscPlugin*> (
                edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}).get()))
        {
            track.pluginList.insertPlugin (*synth, 0, nullptr);

            if (auto volume = track.getVolumePlugin())
                volume->setVolumeDb (-6.0f);
        }
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

            const auto startBeat = placement.getStart();
            const te::BeatRange beats (te::BeatPosition::fromBeats (startBeat),
                                       te::BeatPosition::fromBeats (startBeat + pattern->getLengthBeats()));
            auto midiClip = track.insertMIDIClip (pattern->getName(),
                                                  edit.tempoSequence.toTime (beats), nullptr);
            if (midiClip == nullptr)
                continue;

            for (const auto& note : pattern->getNotes())
                midiClip->getSequence().addNote (note.getPitch(),
                                                 te::BeatPosition::fromBeats (note.getStart()),
                                                 te::BeatDuration::fromBeats (note.getLength()),
                                                 note.getVelocity(), 0, nullptr);
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

        ensureInstrument (edit, track);
        rebuildClips (song, generator, edit, track);
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

} // namespace orionish::sync
