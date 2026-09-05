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

    // Applies the model's sidechain routing to the live effect plugins.
    //
    // The engine does the heavy lifting: any plugin whose sidechainSourceID
    // names a track makes the node builder tap that track post-fader and feed
    // the plugin's third channel onward -- nothing is inserted anywhere. This
    // only has to keep three plugin-state values in step with one model
    // property, compare-before-set so a steady-state resync touches nothing.
    void syncSidechains (const model::Song& song, te::Edit& edit,
                         const juce::Array<te::AudioTrack*>& tracks)
    {
        const auto generators = song.getGenerators();

        auto trackIdFor = [&] (const juce::String& generatorId) -> te::EditItemID
        {
            for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
                if (generators[(size_t) i].getId() == generatorId)
                    return tracks[i]->itemID;

            return {};
        };

        static const juce::Identifier sidechainTriggerId ("sidechainTrigger");

        for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
        {
            for (const auto& effect : generators[(size_t) i].getEffects())
            {
                auto* plugin = findEffectPlugin (*tracks[i], effect.getId());
                if (plugin == nullptr)
                    continue;

                // A source that no longer resolves (the generator was deleted)
                // reads the same as none: the live side is cleared and the
                // compressor falls back to compressing its own input.
                const auto wanted = trackIdFor (effect.getSidechainSourceId());

                if (plugin->getSidechainSourceID() != wanted)
                {
                    plugin->setSidechainSourceID (wanted);

                    // 0->0, 1->1 pass the track through; 2->2 (and 3->2 for a
                    // stereo source) feed the tap into the trigger channel.
                    if (wanted.isValid() && plugin->getNumWires() == 0)
                        plugin->guessSidechainRouting();
                }

                if (auto compressor = dynamic_cast<te::CompressorPlugin*> (plugin))
                {
                    const bool trigger = wanted.isValid();

                    if (compressor->useSidechainTrigger.get() != trigger)
                        compressor->state.setProperty (sidechainTriggerId, trigger, nullptr);
                }
            }
        }
    }

    // The return tracks' own contents: an AuxReturn at the head, then the
    // bus's shared effects, then the track fader the mixer state drives.
    void syncReturns (const model::Song& song, te::Edit& edit)
    {
        static const juce::Identifier busNumId ("busNum");

        for (auto track : te::getAudioTracks (edit))
        {
            const auto returnTrackId = sync::getReturnTrackId (*track);

            if (returnTrackId.isEmpty())
                continue;

            auto ret = song.findReturn (returnTrackId);
            if (! ret)
                continue;   // deleted; track management removes it next pass

            auto auxReturn = track->pluginList.findFirstPluginOfType<te::AuxReturnPlugin>();

            if (auxReturn == nullptr)
            {
                if (auto plugin = edit.getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, {}))
                {
                    track->pluginList.insertPlugin (plugin, 0, nullptr);
                    auxReturn = dynamic_cast<te::AuxReturnPlugin*> (plugin.get());
                }
            }

            if (auxReturn != nullptr && auxReturn->busNumber.get() != ret->getBusNumber())
                auxReturn->state.setProperty (busNumId, ret->getBusNumber(), nullptr);

            if (track->getName() != ret->getName())
                track->setName (ret->getName());

            syncEffectChain (edit, track->pluginList, track->state, ret->getEffects(),
                             [&ret] (const juce::String& id) { return ret->findEffect (id).has_value(); },
                             1);

            if (auto volume = track->getVolumePlugin())
                if (std::abs (volume->getVolumeDb() - ret->getVolumeDb()) > 0.01f)
                    volume->setVolumeDb (ret->getVolumeDb());

            if (track->isMuted (false) != ret->isMuted())
                track->setMute (ret->isMuted());

            // Soloing a generator must not silence the shared reverb it sends
            // into, or solo would never sound like the mix.
            if (! track->isSoloIsolate (false))
                track->setSoloIsolate (true);
        }
    }

    // One AuxSendPlugin per (generator track, sent-to bus), post-fader.
    // Invisible to the rest of the sync: it carries no effect id and is not an
    // instrument type, so findInstrument and syncEffects both pass it by.
    void syncSends (const model::Song& song, te::Edit& edit,
                    te::AudioTrack& track, const model::Generator& generator)
    {
        static const juce::Identifier busNumId ("busNum");

        auto busNumberFor = [&song] (const juce::String& returnId) -> int
        {
            if (auto ret = song.findReturn (returnId))
                return ret->getBusNumber();

            return 0;   // dangling send: the return was deleted
        };

        const auto sends = generator.getSends();

        // Copy: deleting mutates the list. A live send whose bus no model send
        // wants any more goes.
        for (auto plugin : te::Plugin::Array (track.pluginList.getPlugins()))
        {
            auto send = dynamic_cast<te::AuxSendPlugin*> (plugin);
            if (send == nullptr)
                continue;

            const auto wanted = std::any_of (sends.begin(), sends.end(),
                                             [&] (const model::Send& modelSend)
                                             {
                                                 return busNumberFor (modelSend.getReturnId())
                                                          == send->busNumber.get();
                                             });

            if (! wanted)
                send->deleteFromParent();
        }

        for (const auto& modelSend : sends)
        {
            const auto bus = busNumberFor (modelSend.getReturnId());
            if (bus <= 0)
                continue;

            te::AuxSendPlugin* live = nullptr;

            for (auto plugin : track.pluginList.getPlugins())
                if (auto send = dynamic_cast<te::AuxSendPlugin*> (plugin))
                    if (send->busNumber.get() == bus)
                        live = send;

            if (live == nullptr)
            {
                if (auto plugin = edit.getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, {}))
                {
                    // At the end of the list: after the fader, so the send
                    // follows the channel level the way a post-fader send should.
                    track.pluginList.insertPlugin (plugin, track.pluginList.size(), nullptr);
                    live = dynamic_cast<te::AuxSendPlugin*> (plugin.get());

                    if (live != nullptr)
                        live->state.setProperty (busNumId, bus, nullptr);
                }
            }

            // The gain round-trips through the fader taper, so compare loosely
            // -- the same reason applyMixerState does.
            if (live != nullptr && std::abs (live->getGainDb() - modelSend.getGainDb()) > 0.01f)
                live->setGainDb (modelSend.getGainDb());
        }
    }

    // Copies one live effect plugin's state back into its model node --
    // external plugins as base64, internal ones as their own tree.
    void captureEffectState (model::Effect effect, te::Plugin& plugin)
    {
        if (auto external = dynamic_cast<te::ExternalPlugin*> (&plugin))
        {
            if (auto instance = external->getAudioPluginInstance())
            {
                juce::MemoryBlock block;
                instance->getStateInformation (block);
                if (! block.isEmpty())
                    effect.setPluginState (block.toBase64Encoding(), nullptr);
            }

            return;
        }

        // sidechainSourceID is an EditItemID, unique only within this session
        // -- saved as-is it could collide with a different track's id after a
        // reload. The model's own sidechainSource property is the durable
        // form, and the sync rebuilds the live value from it.
        auto captured = plugin.state.createCopy();
        captured.removeProperty (juce::Identifier ("sidechainSourceID"), nullptr);

        // Curves are generated from the model's automation lanes; captured
        // as-is they would come back twice, in seconds that a tempo change
        // has already invalidated. Modifier assignments likewise: they name
        // the modifier by its session-local itemID.
        for (int i = captured.getNumChildren(); --i >= 0;)
        {
            const auto child = captured.getChild (i);

            if (child.hasType (juce::Identifier ("AUTOMATIONCURVE"))
                 || child.hasType (juce::Identifier ("MODIFIERASSIGNMENTS")))
                captured.removeChild (i, nullptr);
        }

        effect.setInternalState (captured, nullptr);
    }

    // Applies the model's automation lanes to the live plugins' curves.
    //
    // Points go through AutomatableParameter::getCurve() rather than into the
    // state tree directly: the parameter binds its curve child at construction
    // and would never notice a tree appended behind its back -- the API's
    // addPoint parents the curve node and wakes the parameter's listeners.
    //
    // The model stores beats, because a curve in seconds detaches from the
    // music at the first tempo change; the conversion happens here, and again
    // whenever the tempo map changes.
    void syncAutomation (const model::Song& song, te::Edit& edit,
                         const juce::Array<te::AudioTrack*>& tracks)
    {
        struct WantedPoint { double seconds; float value, curve; };

        const auto generators = song.getGenerators();
        juce::ignoreUnused (edit);

        for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
        {
            const auto& generator = generators[(size_t) i];
            auto& track = *tracks[i];

            auto resolve = [&] (const model::AutomationLane& lane)
                -> std::pair<te::Plugin*, juce::String>
            {
                const auto target = lane.getTarget();

                if (target == model::AutomationLane::volumeTarget)
                    return { track.getVolumePlugin(), "volume" };

                if (target == model::AutomationLane::panTarget)
                    return { track.getVolumePlugin(), "pan" };

                if (target == model::AutomationLane::instrumentTarget)
                    return { findInstrument (track), lane.getParam() };

                return { findEffectPlugin (track, target), lane.getParam() };
            };

            std::map<te::AutomatableParameter*, std::vector<WantedPoint>> wanted;

            for (const auto& lane : generator.getAutomationLanes())
            {
                auto [plugin, paramId] = resolve (lane);

                if (plugin == nullptr || paramId.isEmpty())
                    continue;

                auto param = plugin->getAutomatableParameterByID (paramId);
                if (param == nullptr)
                    continue;

                auto& points = wanted[param.get()];

                for (const auto& point : lane.getPoints())
                    points.push_back ({ song.secondsFromBeats (point.getBeat()),
                                        point.getValue(), point.getCurve() });
            }

            // Every parameter automation can sit on: the fader pair, the
            // instrument's, and each insert effect's.
            std::vector<te::Plugin*> candidates;
            candidates.push_back (track.getVolumePlugin());
            candidates.push_back (findInstrument (track));

            for (auto plugin : track.pluginList.getPlugins())
                if (isEffect (*plugin))
                    candidates.push_back (plugin);

            for (auto plugin : candidates)
            {
                if (plugin == nullptr)
                    continue;

                for (auto param : plugin->getAutomatableParameters())
                {
                    const auto found = wanted.find (param);
                    const auto* points = found != wanted.end() ? &found->second : nullptr;
                    auto& curve = param->getCurve();

                    // Compare before rewriting: a curve rebuild per resync
                    // would re-trigger the parameter machinery constantly.
                    const auto matches = [&]
                    {
                        const auto count = points != nullptr ? (int) points->size() : 0;

                        if (curve.getNumPoints() != count)
                            return false;

                        for (int n = 0; n < count; ++n)
                        {
                            const auto& want = (*points)[(size_t) n];

                            if (std::abs (curve.getPointTime (n).inSeconds() - want.seconds) > 1.0e-6
                                 || std::abs (curve.getPointValue (n) - want.value) > 1.0e-6f
                                 || std::abs (curve.getPointCurve (n) - want.curve) > 1.0e-6f)
                                return false;
                        }

                        return true;
                    }();

                    if (matches)
                        continue;

                    curve.clear();

                    if (points != nullptr)
                        for (const auto& want : *points)
                            curve.addPoint (te::TimePosition::fromSeconds (want.seconds),
                                            want.value, want.curve);

                    // Curve edits arm the parameter through a 10ms timer,
                    // which never fires without a message loop -- the CLI
                    // renders long before it would. This is the synchronous
                    // version of the same update.
                    param->updateStream();
                }
            }
        }
    }

    // Applies the model's modifiers: one live LFOModifier per model MODIFIER,
    // stamped with its id like effects are, its settings mirrored one to one,
    // and its assignments reconciled through the parameter API -- assignments
    // store the modifier's session-local itemID, so they are never captured
    // and always rebuilt from the model.
    void syncModifiers (const model::Song& song, te::Edit& edit,
                        const juce::Array<te::AudioTrack*>& tracks)
    {
        static const juce::Identifier modifierIdProperty ("carveModifierId");
        static const juce::Identifier lfoType ("LFO");

        const auto generators = song.getGenerators();

        for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
        {
            const auto& generator = generators[(size_t) i];
            auto& track = *tracks[i];
            auto* modifierList = track.getModifierList();

            if (modifierList == nullptr)
                continue;

            const auto modifiers = generator.getModifiers();

            auto findLive = [&] (const juce::String& id) -> te::Modifier*
            {
                for (auto m : modifierList->getModifiers())
                    if (m->state.getProperty (modifierIdProperty).toString() == id)
                        return m;

                return nullptr;
            };

            // Copy: removal mutates the list. Anything the model no longer
            // wants goes, which also drops its assignments.
            for (auto live : modifierList->getModifiers())
            {
                const auto id = live->state.getProperty (modifierIdProperty).toString();
                const auto wanted = std::any_of (modifiers.begin(), modifiers.end(),
                                                 [&id] (const model::GenModifier& m)
                                                 { return m.getId() == id; });

                if (! wanted)
                    live->remove();
            }

            for (const auto& modifier : modifiers)
            {
                if (modifier.getKind() != model::GenModifier::lfoKind)
                    continue;   // the only kind so far

                auto* live = findLive (modifier.getId());

                if (live == nullptr)
                {
                    juce::ValueTree v (lfoType);
                    v.setProperty (modifierIdProperty, modifier.getId(), nullptr);

                    if (auto inserted = modifierList->insertModifier (v, -1, nullptr))
                        live = inserted.get();
                }

                if (live == nullptr)
                    continue;

                // Mirror the settings, compare-before-set. The property names
                // are tracktion's own, so this is a straight copy.
                auto mirror = [&] (const juce::Identifier& prop, const juce::var& value)
                {
                    if (live->state.getProperty (prop) != value)
                        live->state.setProperty (prop, value, nullptr);
                };

                mirror (juce::Identifier ("rate"), modifier.getRate());
                mirror (juce::Identifier ("rateType"), modifier.getRateType());
                mirror (juce::Identifier ("depth"), modifier.getDepth());
                mirror (juce::Identifier ("wave"), modifier.getWave());
                mirror (juce::Identifier ("syncType"), modifier.getSyncType());
                mirror (juce::Identifier ("bipolar"), modifier.isBipolar());
                mirror (juce::Identifier ("phase"), modifier.getPhase());
                mirror (juce::Identifier ("offset"), modifier.getOffset());

                // The settings are exposed as AutomatableParameters attached
                // to those properties, and processing reads the parameter, not
                // the tree -- the same lesson the Distortion restore taught:
                // without this the LFO runs at its construction defaults.
                for (auto param : live->getAutomatableParameters())
                    param->updateFromAttachedValue();

                // Assignments: same target addressing as automation lanes.
                auto resolve = [&] (const model::ModifierAssign& assign)
                    -> te::AutomatableParameter::Ptr
                {
                    const auto target = assign.getTarget();
                    te::Plugin* plugin = nullptr;
                    juce::String paramId;

                    if (target == model::AutomationLane::volumeTarget)
                        { plugin = track.getVolumePlugin(); paramId = "volume"; }
                    else if (target == model::AutomationLane::panTarget)
                        { plugin = track.getVolumePlugin(); paramId = "pan"; }
                    else if (target == model::AutomationLane::instrumentTarget)
                        { plugin = findInstrument (track); paramId = assign.getParam(); }
                    else
                        { plugin = findEffectPlugin (track, target); paramId = assign.getParam(); }

                    if (plugin == nullptr || paramId.isEmpty())
                        return {};

                    return plugin->getAutomatableParameterByID (paramId);
                };

                for (const auto& assign : modifier.getAssigns())
                {
                    if (auto param = resolve (assign))
                    {
                        // addModifier answers the existing assignment if one
                        // is already there, so this is idempotent by itself.
                        if (auto assignment = param->addModifier (*live, assign.getAmount()))
                            if (std::abs (assignment->value.get() - assign.getAmount()) > 1.0e-6f)
                                assignment->value = assign.getAmount();

                        param->updateStream();
                    }
                }
            }
        }
    }

    // One MIDI note as a placement wants it, expanded for looping, transpose
    // and cut-off. The comparison against a live clip and the rewrite of one
    // both walk this list, so they can't disagree.
    struct WantedNote
    {
        int pitch;
        double startBeats, lengthBeats;
        int velocity;
    };

    std::vector<WantedNote> wantedNotesFor (const model::Pattern& pattern, double patternLength,
                                            double clipLength, int transpose)
    {
        std::vector<WantedNote> wanted;
        const auto notes = pattern.getNotes();

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

                wanted.push_back ({ model::Note::clampPitch (note.getPitch() + transpose),
                                    noteStart,
                                    std::min (note.getLength(), clipLength - noteStart),
                                    note.getVelocity() });
            }
        }

        return wanted;
    }

    // Compared as sets: the MidiList keeps its notes sorted by start beat with
    // an unspecified order for ties, and the wanted list is in pattern order,
    // so both sides are put into one canonical order first.
    bool sequenceMatches (const te::MidiClip& clip, std::vector<WantedNote> wanted)
    {
        const auto& liveNotes = clip.getSequence().getNotes();

        if ((size_t) liveNotes.size() != wanted.size())
            return false;

        std::vector<WantedNote> live;
        live.reserve (wanted.size());

        for (const auto* note : liveNotes)
            live.push_back ({ note->getNoteNumber(), note->getStartBeat().inBeats(),
                              note->getLengthBeats().inBeats(), note->getVelocity() });

        const auto order = [] (const WantedNote& a, const WantedNote& b)
        {
            return std::tie (a.startBeats, a.pitch, a.lengthBeats, a.velocity)
                 < std::tie (b.startBeats, b.pitch, b.lengthBeats, b.velocity);
        };
        std::sort (live.begin(), live.end(), order);
        std::sort (wanted.begin(), wanted.end(), order);

        for (size_t i = 0; i < wanted.size(); ++i)
            if (live[i].pitch != wanted[i].pitch
                 || live[i].velocity != wanted[i].velocity
                 || std::abs (live[i].startBeats - wanted[i].startBeats) > 1.0e-6
                 || std::abs (live[i].lengthBeats - wanted[i].lengthBeats) > 1.0e-6)
                return false;

        return true;
    }

    // Reconciles the track's MIDI clips against the playlist instead of
    // deleting and recreating them. This is not (only) an optimisation:
    // inserting or removing a te::Clip makes tracktion rebuild the whole
    // playback graph, and a rebuild a few milliseconds after a note preview
    // races the preview's voice -- which is how "add a note, hear nothing,
    // add another, hear that one" happened. Rewriting a clip's MidiList
    // touches no graph, so the common edit (notes changing inside a pattern)
    // leaves playback and previews alone.
    //
    // Clips are matched positionally: every clip on a generator track is ours
    // and created in playlist order, the same invariant repositionPatternClips
    // already relies on.
    void rebuildClips (const model::Song& song, const model::Generator& generator,
                       te::Edit& edit, te::AudioTrack& track)
    {
        struct Desired
        {
            juce::String name;
            te::TimeRange time;
            std::vector<WantedNote> notes;
        };

        std::vector<Desired> desired;

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

            desired.push_back ({ pattern->getName(),
                                 edit.tempoSequence.toTime (beats),
                                 wantedNotesFor (*pattern, patternLength, clipLength,
                                                 placement.getTranspose()) });
        }

        // The live clips, positionally. Anything that is not one of our MIDI
        // clips (there should be none on a generator track) is removed rather
        // than reasoned about.
        std::vector<te::MidiClip*> live;

        for (auto clip : juce::Array<te::Clip*> (track.getClips()))   // copy: removal mutates the list
        {
            if (auto midi = dynamic_cast<te::MidiClip*> (clip); midi != nullptr && live.size() < desired.size())
                live.push_back (midi);
            else
                clip->removeFromParent();
        }

        for (size_t i = 0; i < desired.size(); ++i)
        {
            const auto& want = desired[i];
            te::MidiClip* midiClip = nullptr;

            if (i < live.size())
            {
                midiClip = live[i];
            }
            else
            {
                midiClip = track.insertMIDIClip (want.name, want.time, nullptr).get();

                if (midiClip == nullptr)
                    continue;
            }

            if (midiClip->getName() != want.name)
                midiClip->setName (want.name);

            if (! positionsMatch (midiClip->getPosition(), { want.time, te::TimeDuration() }))
                midiClip->setPosition ({ want.time, te::TimeDuration() });

            if (! sequenceMatches (*midiClip, want.notes))
            {
                auto& sequence = midiClip->getSequence();
                sequence.clear (nullptr);

                for (const auto& note : want.notes)
                    sequence.addNote (note.pitch,
                                      te::BeatPosition::fromBeats (note.startBeats),
                                      te::BeatDuration::fromBeats (note.lengthBeats),
                                      note.velocity, 0, nullptr);
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
    const auto returns = song.getReturns();

    // Track layout: generator tracks first, in generator order, then one track
    // per return bus. Return tracks are told apart by a stamp, never by
    // position -- an index shift must not point the generator sync at a track
    // full of shared reverb, which is how the first attempt at this died.
    {
        auto all = te::getAudioTracks (edit);

        // Return tracks whose bus is gone, then surplus generator tracks from
        // the end (preserving generator order).
        for (int i = all.size(); --i >= 0;)
            if (sync::isReturnTrack (*all[i])
                 && ! song.findReturn (sync::getReturnTrackId (*all[i])))
                edit.deleteTrack (all[i]);

        auto generatorTracks = [&edit]
        {
            juce::Array<te::AudioTrack*> result;

            for (auto track : te::getAudioTracks (edit))
                if (! sync::isReturnTrack (*track))
                    result.add (track);

            return result;
        };

        for (auto current = generatorTracks(); current.size() > (int) generators.size();
             current = generatorTracks())
            edit.deleteTrack (current.getLast());

        while (generatorTracks().size() < (int) generators.size())
            edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr);

        for (const auto& ret : returns)
        {
            bool exists = false;

            for (auto track : te::getAudioTracks (edit))
                if (sync::getReturnTrackId (*track) == ret.getId())
                    exists = true;

            if (! exists)
                if (auto track = edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr))
                    track->state.setProperty (sync::returnIdProperty, ret.getId(), nullptr);
        }

        // Order: a generator added while returns exist appears at the very
        // end, behind them; move any return track that is not behind every
        // generator track. Steady state makes no moves and rebuilds nothing.
        for (bool moved = true; moved; )
        {
            moved = false;
            auto all2 = te::getAudioTracks (edit);

            for (int i = 0; i < all2.size() - 1; ++i)
            {
                if (sync::isReturnTrack (*all2[i]) && ! sync::isReturnTrack (*all2[i + 1]))
                {
                    edit.moveTrack (all2[i], te::TrackInsertPoint (nullptr, all2[all2.size() - 1]));
                    moved = true;
                    break;
                }
            }
        }
    }

    // Everything below addresses generator tracks only.
    juce::Array<te::AudioTrack*> tracks;

    for (auto track : te::getAudioTracks (edit))
        if (! sync::isReturnTrack (*track))
            tracks.add (track);

    for (int i = 0; i < (int) generators.size(); ++i)
    {
        const auto& generator = generators[(size_t) i];
        auto& track = *tracks[i];

        if (track.getName() != generator.getName())
            track.setName (generator.getName());

        ensureInstrument (edit, track, generator);
        syncEffects (edit, track, generator);
        syncSends (song, edit, track, generator);
        applyMixerState (generator, track);

        if (generator.isAudio())
            syncAudioClips (song, generator, edit, track);
        else
            rebuildClips (song, generator, edit, track);
    }

    syncSidechains (song, edit, tracks);
    syncAutomation (song, edit, tracks);
    syncModifiers (song, edit, tracks);
    syncReturns (song, edit);

}

void flushSamplerLoads (te::Edit& edit)
{
    constexpr int maxAttempts = 200;

    for (int attempt = 0; attempt < maxAttempts && ! samplerSoundsAreLoaded (edit); ++attempt)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
}

void EditSync::captureLivePluginState()
{
    // Return busses first: their reverbs have knobs too.
    for (auto track : te::getAudioTracks (edit))
    {
        const auto returnTrackId = sync::getReturnTrackId (*track);
        if (returnTrackId.isEmpty())
            continue;

        if (auto ret = song.findReturn (returnTrackId))
            for (auto effect : ret->getEffects())
                if (auto plugin = findEffectPlugin (*track, effect.getId()))
                    captureEffectState (effect, *plugin);
    }

    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
    {
        auto generator = generators[(size_t) i];

        // Insert effects first: these apply whatever the generator's own type.
        for (auto effect : generator.getEffects())
            if (auto plugin = findEffectPlugin (*tracks[i], effect.getId()))
                captureEffectState (effect, *plugin);

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

    // Automation curves are written to the engine in seconds, so a tempo
    // change moves every point's wall-clock position.
    applyAutomationOnly();
}

void EditSync::applySendsAndReturnsOnly()
{
    syncReturns (song, edit);

    const auto generators = song.getGenerators();
    juce::Array<te::AudioTrack*> generatorTracks;

    for (auto track : te::getAudioTracks (edit))
        if (! sync::isReturnTrack (*track))
            generatorTracks.add (track);

    for (int i = 0; i < (int) generators.size() && i < generatorTracks.size(); ++i)
        syncSends (song, edit, *generatorTracks[i], generators[(size_t) i]);
}

void EditSync::applyAutomationOnly()
{
    juce::Array<te::AudioTrack*> generatorTracks;

    for (auto track : te::getAudioTracks (edit))
        if (! sync::isReturnTrack (*track))
            generatorTracks.add (track);

    syncAutomation (song, edit, generatorTracks);
}

void EditSync::applyMixerStateOnly()
{
    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
        applyMixerState (generators[(size_t) i], *tracks[i]);
}

} // namespace carve::sync
