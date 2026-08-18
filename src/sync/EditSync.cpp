#include "EditSync.h"

#include "EngineIds.h"

namespace carve::sync
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

    using sync::effectIdProperty;
    using sync::getEffectId;

    bool isEffect (const te::Plugin& plugin)  { return sync::isEffectPlugin (plugin); }

    // An insert effect can be an ExternalPlugin too, so the instrument is
    // whichever unstamped one comes first.
    bool isInstrument (const te::Plugin& plugin)
    {
        return ! isEffect (plugin)
                && (dynamic_cast<const te::FourOscPlugin*> (&plugin) != nullptr
                     || dynamic_cast<const te::ExternalPlugin*> (&plugin) != nullptr
                     || dynamic_cast<const te::SamplerPlugin*> (&plugin) != nullptr);
    }

    void removeInstruments (te::AudioTrack& track)
    {
        for (auto plugin : track.pluginList.getPlugins())
            if (isInstrument (*plugin))
                plugin->deleteFromParent();
    }

    te::Plugin* findInstrument (te::AudioTrack& track)  { return sync::findInstrumentPlugin (track); }

    te::Plugin* findEffectPlugin (te::PluginList& plugins, const juce::String& effectId)
    {
        for (auto plugin : plugins.getPlugins())
            if (getEffectId (*plugin) == effectId)
                return plugin;

        return nullptr;
    }

    te::Plugin* findEffectPlugin (te::AudioTrack& track, const juce::String& effectId)
    {
        return findEffectPlugin (track.pluginList, effectId);
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
        if (auto existing = dynamic_cast<te::ExternalPlugin*> (findInstrument (track)))
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
        if (dynamic_cast<te::FourOscPlugin*> (findInstrument (track)) != nullptr)
            return;

        removeInstruments (track);

        if (auto synth = dynamic_cast<te::FourOscPlugin*> (
                edit.getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}).get()))
            track.pluginList.insertPlugin (*synth, 0, nullptr);
    }

    // SamplerPlugin keeps its sounds as SOUND children of its own state tree,
    // but only exposes their media path through a list it rebuilds from an
    // async callback. Reading the tree directly is what lets a resync tell an
    // unchanged sound from a changed one -- and tell whether that rebuild has
    // happened yet, which flushSamplerLoads below needs.
    const juce::Identifier samplerSoundType ("SOUND");
    const juce::Identifier samplerSourceProperty ("source");

    juce::String getSoundSource (const te::SamplerPlugin& sampler, int soundIndex)
    {
        int index = 0;

        for (const auto& child : sampler.state)
            if (child.hasType (samplerSoundType) && index++ == soundIndex)
                return child[samplerSourceProperty].toString();

        return {};
    }

    // Reconciles the plugin's sounds against the model in place. Re-pointing a
    // sound the sampler already has is free, where removing and re-adding it
    // would throw away the audio it has read off disk and cut whatever it is
    // playing -- the same reason the effect chain matches instead of rebuilds.
    void syncSamplerSounds (te::SamplerPlugin& sampler, const model::Generator& generator)
    {
        const auto sounds = generator.getSounds();

        // Matched by source file rather than by index. A sampler's sounds are
        // addressed by key range, so their order means nothing to playback --
        // but matching positionally meant clearing one pad shifted every later
        // sound down and re-pointed its media, which makes the sampler read
        // those files off disk again for no reason.
        std::vector<bool> claimed ((size_t) sampler.getNumSounds(), false);

        auto claimExisting = [&] (const juce::String& path) -> int
        {
            for (int i = 0; i < sampler.getNumSounds(); ++i)
                if (! claimed[(size_t) i] && getSoundSource (sampler, i) == path)
                {
                    claimed[(size_t) i] = true;
                    return i;
                }

            return -1;
        };

        for (const auto& sound : sounds)
        {
            const auto path = sound.getFile().getFullPathName();
            auto index = claimExisting (path);

            if (index < 0)
            {
                // Start 0 / length 0 is the whole file: trimming a sample is
                // not something the model describes yet. A non-empty return
                // means the sampler is full, and so are we.
                if (sampler.addSound (path, sound.getName(), 0.0, 0.0, sound.getGainDb()).isNotEmpty())
                    break;

                index = sampler.getNumSounds() - 1;
                claimed.resize ((size_t) sampler.getNumSounds(), false);
                claimed[(size_t) index] = true;
            }

            if (sampler.getSoundName (index) != sound.getName())
                sampler.setSoundName (index, sound.getName());

            if (sampler.getKeyNote (index) != sound.getRootNote()
                 || sampler.getMinKey (index) != sound.getMinNote()
                 || sampler.getMaxKey (index) != sound.getMaxNote())
                sampler.setSoundParams (index, sound.getRootNote(), sound.getMinNote(), sound.getMaxNote());

            if (std::abs (sampler.getSoundGainDb (index) - sound.getGainDb()) > 0.01f
                 || std::abs (sampler.getSoundPan (index) - sound.getPan()) > 0.001f)
                sampler.setSoundGains (index, sound.getGainDb(), sound.getPan());
        }

        // Whatever nothing in the model claimed is gone; high to low so the
        // indices stay valid as they are removed.
        for (int i = (int) claimed.size(); --i >= 0;)
            if (! claimed[(size_t) i])
                sampler.removeSound (i);
    }

    void ensureSamplerInstrument (te::Edit& edit, te::AudioTrack& track,
                                  const model::Generator& generator)
    {
        auto sampler = dynamic_cast<te::SamplerPlugin*> (findInstrument (track));

        if (sampler == nullptr)
        {
            removeInstruments (track);

            auto plugin = edit.getPluginCache().createNewPlugin (te::SamplerPlugin::xmlTypeName, {});
            sampler = dynamic_cast<te::SamplerPlugin*> (plugin.get());

            if (sampler == nullptr)
                return;

            track.pluginList.insertPlugin (plugin, 0, nullptr);
        }

        syncSamplerSounds (*sampler, generator);
    }

    void ensureInstrument (te::Edit& edit, te::AudioTrack& track, const model::Generator& generator)
    {
        if (generator.isAudio())
        {
            // Nothing generates on an audio track: its wave clips already are
            // the sound. Still worth clearing, so retyping a generator to
            // "audio" doesn't leave the old synth sitting in the chain.
            removeInstruments (track);
            return;
        }

        if (generator.isSampler())
        {
            ensureSamplerInstrument (edit, track, generator);
            return;
        }

        if (generator.getType() == "plugin")
        {
            if (auto description = generator.getPluginDescription())
                ensureExternalInstrument (edit, track, generator, *description);
            return;
        }
        ensureInternalInstrument (edit, track);
    }

    bool samplerSoundsAreLoaded (te::Edit& edit)
    {
        for (auto track : te::getAudioTracks (edit))
            if (auto sampler = track->pluginList.findFirstPluginOfType<te::SamplerPlugin>())
                for (int i = 0; i < sampler->getNumSounds(); ++i)
                    if (getSoundSource (*sampler, i).isNotEmpty()
                         && sampler->getSoundMedia (i).isEmpty())
                        return false;

        return true;
    }

    te::Plugin::Ptr createEffectPlugin (te::Edit& edit, const model::Effect& effect)
    {
        te::Plugin::Ptr plugin;

        if (effect.isExternal())
        {
            if (auto description = effect.getPluginDescription())
                plugin = edit.getPluginCache().createNewPlugin (
                    te::ExternalPlugin::create (edit.engine, *description));
        }
        else if (auto stored = effect.getInternalState(); stored.isValid())
        {
            // Rebuild it from the state we saved, so its parameters come back.
            plugin = edit.getPluginCache().createNewPlugin (stored.createCopy());
        }
        else
        {
            plugin = edit.getPluginCache().createNewPlugin (effect.getType(), {});
        }

        if (plugin != nullptr)
        {
            plugin->state.setProperty (effectIdProperty, effect.getId(), nullptr);

            if (auto external = dynamic_cast<te::ExternalPlugin*> (plugin.get()))
                restorePluginState (*external, effect.getPluginState());
        }

        return plugin;
    }

    // Puts the effect plugins into the model's order. insertPlugin can't move a
    // plugin that is already in the list, and deleting and re-adding one would
    // close its editor window, so the plugins' own ValueTrees are reordered
    // instead -- which also means the live plugin objects, and everything the
    // user has tweaked on them, are left alone.
    void orderEffects (juce::ValueTree ownerState, const std::vector<model::Effect>& effects)
    {
        auto trackState = ownerState;

        for (size_t position = 0; position < effects.size(); ++position)
        {
            // Recomputed each time: a move shifts everything between.
            juce::Array<int> effectSlots;
            juce::Array<juce::ValueTree> effectTrees;

            for (int i = 0; i < trackState.getNumChildren(); ++i)
            {
                auto child = trackState.getChild (i);

                if (child.hasProperty (effectIdProperty))
                {
                    effectSlots.add (i);
                    effectTrees.add (child);
                }
            }

            if ((int) position >= effectSlots.size())
                break;

            const auto wantedId = effects[position].getId();
            int found = -1;

            for (int i = (int) position; i < effectTrees.size(); ++i)
                if (effectTrees[i].getProperty (effectIdProperty).toString() == wantedId)
                    found = i;

            if (found < 0 || found == (int) position)
                continue;

            trackState.moveChild (effectSlots[found], effectSlots[position], nullptr);
        }
    }

    // Reconciles one plugin list against one model effect list. Shared by the
    // generator chains and the master chain, which differ only in where their
    // effects sit and what they sit after.
    void syncEffectChain (te::Edit& edit, te::PluginList& plugins, juce::ValueTree ownerState,
                          const std::vector<model::Effect>& effects,
                          const std::function<bool (const juce::String&)>& stillWanted,
                          int insertAt)
    {
        // Copy: deleting mutates the list we would be walking.
        for (auto plugin : te::Plugin::Array (plugins.getPlugins()))
            if (isEffect (*plugin) && ! stillWanted (getEffectId (*plugin)))
                plugin->deleteFromParent();

        for (const auto& effect : effects)
        {
            if (findEffectPlugin (plugins, effect.getId()) != nullptr)
                continue;

            if (auto plugin = createEffectPlugin (edit, effect))
                plugins.insertPlugin (plugin, insertAt, nullptr);
        }

        orderEffects (ownerState, effects);

        for (const auto& effect : effects)
            if (auto plugin = findEffectPlugin (plugins, effect.getId()))
                if (plugin->isEnabled() != effect.isEnabled())
                    plugin->setEnabled (effect.isEnabled());
    }

    void syncEffects (te::Edit& edit, te::AudioTrack& track, const model::Generator& generator)
    {
        // After the instrument, before the fader: the level meter is post-fader
        // and stays that way.
        const auto instrument = findInstrument (track);
        const auto insertAt = instrument != nullptr ? track.pluginList.indexOf (instrument) + 1 : 0;

        syncEffectChain (edit, track.pluginList, track.state, generator.getEffects(),
                         [&generator] (const juce::String& id) { return generator.findEffect (id).has_value(); },
                         insertAt);
    }

    void syncMasterBus (const model::Song& song, te::Edit& edit)
    {
        const auto master = song.getMasterBus();

        // Master effects go at the head of the list, so the Edit's own master
        // volume and meter stay last and stay post-fader.
        syncEffectChain (edit, edit.getMasterPluginList(), edit.state, master.getEffects(),
                         [&master] (const juce::String& id) { return master.findEffect (id).has_value(); },
                         0);

        if (auto volume = edit.getMasterVolumePlugin())
            if (std::abs (volume->getVolumeDb() - master.getVolumeDb()) > 0.01f)
                volume->setVolumeDb (master.getVolumeDb());
    }

    // Stamped onto the wave clip built for a model AUDIOCLIP, the way an
    // effect plugin is stamped with its Effect's id: it is what lets a resync
    // recognise a placement it has already built and leave it -- and the audio
    // tracktion has read off disk for it -- alone.
    const juce::Identifier audioClipIdProperty ("carveAudioClipId");

    juce::String getAudioClipId (const te::Clip& clip)
    {
        return clip.state.getProperty (audioClipIdProperty).toString();
    }

    // Where a placement sits on the timeline, in the Edit's own units. The
    // offset is measured from beat zero rather than from the clip's start:
    // both give the same duration at a constant tempo, and it can't come out
    // negative for a clip trimmed further than its own start position.
    te::ClipPosition toClipPosition (te::Edit& edit, const model::AudioClip& placement)
    {
        // The start is musical, so it moves with the tempo; the length and the
        // offset describe a span of the source file, which no tempo change can
        // stretch, so they are already the units the Edit wants.
        const auto start = edit.tempoSequence.toTime (te::BeatPosition::fromBeats (placement.getStart()));

        return { te::TimeRange (start, te::TimeDuration::fromSeconds (placement.getLengthSeconds())),
                 te::TimeDuration::fromSeconds (placement.getOffsetSeconds()) };
    }

    // Times round-trip through the tempo sequence, so compare with a tolerance
    // rather than re-setting (and re-notifying) the position on every sync.
    bool positionsMatch (const te::ClipPosition& a, const te::ClipPosition& b)
    {
        constexpr double tolerance = 1.0e-6;

        return std::abs ((a.getStart() - b.getStart()).inSeconds()) < tolerance
                && std::abs ((a.getLength() - b.getLength()).inSeconds()) < tolerance
                && std::abs ((a.getOffset() - b.getOffset()).inSeconds()) < tolerance;
    }

    // Reconciles an audio generator's track against its placements, in the
    // spirit of the effect chain: only what changed is touched. Re-inserting a
    // wave clip would throw away the file tracktion has already opened and cut
    // whatever it is playing, and a placement is re-synced every time anything
    // else in the song is edited.
    void syncAudioClips (const model::Song& song, const model::Generator& generator,
                         te::Edit& edit, te::AudioTrack& track)
    {
        std::vector<model::AudioClip> placements;

        for (const auto& clip : song.getPlaylist().getAudioClips())
            if (clip.getGeneratorId() == generator.getId() && clip.getFile().existsAsFile())
            {
                // Without an id there is nothing to match a live clip by, so
                // this one would be torn down and rebuilt on every resync --
                // worse than not playing. Song::ensureAudioClipIds gives every
                // loaded placement one; this covers a song built in memory.
                jassert (clip.getId().isNotEmpty());

                if (clip.getId().isNotEmpty())
                    placements.push_back (clip);
            }

        // Copy: removal mutates the list we would be walking. Anything without
        // a placement goes, which also clears the MIDI clips left behind by a
        // generator that was an instrument until a moment ago.
        for (auto clip : juce::Array<te::Clip*> (track.getClips()))
        {
            const auto id = getAudioClipId (*clip);
            const auto stillWanted = id.isNotEmpty()
                                      && std::any_of (placements.begin(), placements.end(),
                                                      [&] (const model::AudioClip& p) { return p.getId() == id; });

            if (! stillWanted)
                clip->removeFromParent();
        }

        for (const auto& placement : placements)
        {
            const auto position = toClipPosition (edit, placement);
            te::WaveAudioClip* existing = nullptr;

            for (auto clip : track.getClips())
                if (getAudioClipId (*clip) == placement.getId())
                    if (auto wave = dynamic_cast<te::WaveAudioClip*> (clip))
                    {
                        existing = wave;
                        break;
                    }

            if (existing == nullptr)
            {
                // insertWaveClip reads the file to work out what it is looking
                // at, so this is the one part of a sync that touches the disk.
                auto wave = track.insertWaveClip (placement.getName(), placement.getFile(),
                                                  position, false);
                if (wave == nullptr)
                    continue;

                wave->state.setProperty (audioClipIdProperty, placement.getId(), nullptr);
                continue;
            }

            if (existing->getSourceFileReference().getFile() != placement.getFile())
                existing->getSourceFileReference().setToDirectFileReference (placement.getFile(), false);

            if (! positionsMatch (existing->getPosition(), position))
                existing->setPosition (position);

            if (existing->getName() != placement.getName())
                existing->setName (placement.getName());
        }
    }

    // Moves the pattern clips a tempo change displaced, without rebuilding them.
    //
    // A tempo change alters where every clip sits in time but not which clips
    // exist or what is in them, and a full resync tears down and refills every
    // MIDI clip in the song -- once per beat crossed while a tempo marker is
    // being dragged, during playback. The clips are matched positionally
    // because rebuildClips creates them in this same order and a tempo change
    // cannot have reordered them; if the counts disagree, something structural
    // did change after all and this bails out to let a full resync handle it.
    bool repositionPatternClips (const model::Song& song, const model::Generator& generator,
                                 te::Edit& edit, te::AudioTrack& track)
    {
        std::vector<te::MidiClip*> midiClips;

        for (auto clip : track.getClips())
            if (auto midi = dynamic_cast<te::MidiClip*> (clip))
                midiClips.push_back (midi);

        size_t index = 0;

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

            if (index >= midiClips.size())
                return false;

            const auto startBeat = placement.getStart();
            const te::BeatRange beats (te::BeatPosition::fromBeats (startBeat),
                                       te::BeatPosition::fromBeats (startBeat + clipLength));
            const te::ClipPosition wanted { edit.tempoSequence.toTime (beats), te::TimeDuration() };

            if (! positionsMatch (midiClips[index]->getPosition(), wanted))
                midiClips[index]->setPosition (wanted);

            ++index;
        }

        return index == midiClips.size();
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
    // Rebuilds the Edit's tempo sequence from the model. Everything downstream
    // is positioned in beats and converted through this sequence, so it has to
    // be right before a single clip is placed.
    //
    // remapEdit is false throughout: it exists to drag an Edit's clips along
    // with a tempo change, and ours are rebuilt from the model straight after,
    // so letting it move them too would apply the change twice.
    void syncTempoSequence (const model::Song& song, te::Edit& edit)
    {
        auto& sequence = edit.tempoSequence;

        for (int i = sequence.getNumTempos(); --i > 0;)
            sequence.removeTempo (i, false);

        if (auto first = sequence.getTempo (0))
            if (first->getBpm() != song.getTempo())
                first->setBpm (song.getTempo());

        for (const auto& change : song.getTempoChanges())
        {
            // A change at beat zero replaces the sequence's own first tempo
            // rather than adding a second one on top of it.
            if (change.getStartBeat() <= 0.0)
            {
                if (auto first = sequence.getTempo (0))
                    first->setBpm (change.getBpm());

                continue;
            }

            sequence.insertTempo (te::BeatPosition::fromBeats (change.getStartBeat()),
                                  change.getBpm(), 1.0f);
        }

        for (int i = sequence.getNumTimeSigs(); --i > 0;)
            sequence.removeTimeSig (i);

        auto applySignature = [] (te::TimeSigSetting& setting, model::TimeSignature sig)
        {
            setting.numerator = sig.numerator;
            setting.denominator = sig.denominator;
        };

        if (auto first = sequence.getTimeSig (0))
            applySignature (*first, song.getTimeSigAt (0.0));

        for (const auto& change : song.getTimeSigChanges())
        {
            if (change.getStartBeat() <= 0.0)
                continue;   // already applied as the sequence's first

            if (auto inserted = sequence.insertTimeSig (te::BeatPosition::fromBeats (change.getStartBeat())))
                applySignature (*inserted, change.getSignature());
        }
    }

} // namespace

void syncSongToEdit (const model::Song& song, te::Edit& edit)
{
    syncTempoSequence (song, edit);
    syncMasterBus (song, edit);

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
        syncEffects (edit, track, generator);
        applyMixerState (generator, track);

        if (generator.isAudio())
            syncAudioClips (song, generator, edit, track);
        else
            rebuildClips (song, generator, edit, track);
    }

}

void flushSamplerLoads (te::Edit& edit)
{
    constexpr int maxAttempts = 200;

    for (int attempt = 0; attempt < maxAttempts && ! samplerSoundsAreLoaded (edit); ++attempt)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
}

void EditSync::captureLivePluginState()
{
    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
    {
        auto generator = generators[(size_t) i];

        // Insert effects first: these apply whatever the generator's own type.
        for (auto effect : generator.getEffects())
        {
            auto plugin = findEffectPlugin (*tracks[i], effect.getId());
            if (plugin == nullptr)
                continue;

            if (auto external = dynamic_cast<te::ExternalPlugin*> (plugin))
            {
                if (auto instance = external->getAudioPluginInstance())
                {
                    juce::MemoryBlock block;
                    instance->getStateInformation (block);
                    if (! block.isEmpty())
                        effect.setPluginState (block.toBase64Encoding(), nullptr);
                }
            }
            else
            {
                effect.setInternalState (plugin->state, nullptr);
            }
        }

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

void EditSync::applyTempoOnly()
{
    syncTempoSequence (song, edit);

    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
    {
        const auto& generator = generators[(size_t) i];

        if (! repositionPatternClips (song, generator, edit, *tracks[i]))
        {
            // The clips are not what this path assumed, so fall back rather
            // than leave them where the old tempo put them.
            triggerAsyncUpdate();
            return;
        }

        // Audio placements move too: their start is musical even though their
        // length is not. syncAudioClips already only writes what differs.
        syncAudioClips (song, generator, edit, *tracks[i]);
    }
}

void EditSync::applyMixerStateOnly()
{
    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
        applyMixerState (generators[(size_t) i], *tracks[i]);
}

} // namespace carve::sync
