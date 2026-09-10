#include "EffectChains.h"

#include <algorithm>

#include "EngineIds.h"

namespace carve::sync
{

te::Plugin* EffectChainSite::findPlugin (const juce::String& effectId) const
{
    if (plugins == nullptr || effectId.isEmpty())
        return nullptr;

    for (auto plugin : plugins->getPlugins())
        if (getEffectId (*plugin) == effectId)
            return plugin;

    return nullptr;
}

//==============================================================================
EffectChainSite generatorChainSite (const model::Generator& generator, te::AudioTrack& track)
{
    // After the instrument, before the fader: the level meter is post-fader
    // and stays that way.
    const auto instrument = findInstrumentPlugin (track);

    EffectChainSite site;
    site.ownerId = generator.getId();
    site.effects = generator.getEffects();
    site.plugins = &track.pluginList;
    site.ownerState = track.state;
    site.insertAt = instrument != nullptr ? track.pluginList.indexOf (instrument) + 1 : 0;
    return site;
}

EffectChainSite returnChainSite (const model::Return& bus, te::AudioTrack& track)
{
    // After the aux return, which is what the sends arrive through.
    EffectChainSite site;
    site.ownerId = bus.getId();
    site.effects = bus.getEffects();
    site.plugins = &track.pluginList;
    site.ownerState = track.state;
    site.insertAt = 1;
    return site;
}

EffectChainSite masterChainSite (const model::MasterBus& bus, te::Edit& edit)
{
    // At the head, so the Edit's own master volume stays last.
    EffectChainSite site;
    site.effects = bus.getEffects();
    site.plugins = &edit.getMasterPluginList();
    site.ownerState = edit.state;
    site.insertAt = 0;
    return site;
}

//==============================================================================
std::vector<EffectChainSite> getEffectChainSites (const model::Song& song, te::Edit& edit)
{
    std::vector<EffectChainSite> sites;

    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (edit);

    // Generator order == track order, and return tracks are appended after
    // them: the same invariant the sync itself runs on.
    for (int i = 0; i < (int) generators.size(); ++i)
    {
        if (i < tracks.size())
        {
            sites.push_back (generatorChainSite (generators[(size_t) i], *tracks[i]));
        }
        else
        {
            // The track is not there yet. The site still exists as far as the
            // model is concerned, and saying so is what keeps a caller from
            // quietly skipping a chain it should have seen.
            EffectChainSite site;
            site.ownerId = generators[(size_t) i].getId();
            site.effects = generators[(size_t) i].getEffects();
            sites.push_back (std::move (site));
        }
    }

    for (const auto& bus : song.getReturns())
    {
        te::AudioTrack* returnTrack = nullptr;

        for (auto track : tracks)
            if (getReturnTrackId (*track) == bus.getId())
                returnTrack = track;

        if (returnTrack != nullptr)
        {
            sites.push_back (returnChainSite (bus, *returnTrack));
        }
        else
        {
            EffectChainSite site;
            site.ownerId = bus.getId();
            site.effects = bus.getEffects();
            sites.push_back (std::move (site));
        }
    }

    sites.push_back (masterChainSite (song.getMasterBus(), edit));

    return sites;
}

std::optional<EffectChainSite> findEffectChainSite (const model::Song& song, te::Edit& edit,
                                                    const juce::String& ownerId)
{
    for (auto& site : getEffectChainSites (song, edit))
        if (site.ownerId == ownerId)
            return site;

    return std::nullopt;
}

std::vector<juce::String> findSyncProblems (const model::Song& song, te::Edit& edit)
{
    std::vector<juce::String> problems;

    auto describe = [] (const EffectChainSite& site)
    {
        return site.ownerId.isEmpty() ? juce::String ("the master chain")
                                      : "chain of owner " + site.ownerId;
    };

    for (auto& site : getEffectChainSites (song, edit))
    {
        // A generator added this tick has no track until the next sync, which
        // is a state the app passes through rather than one it rests in.
        if (site.plugins == nullptr)
            continue;

        for (const auto& effect : site.effects)
        {
            if (effect.isExternal())
                continue;   // may be a plugin this machine does not have

            if (site.findPlugin (effect.getId()) == nullptr)
                problems.push_back ("effect " + effect.getType() + " (" + effect.getId()
                                     + ") in " + describe (site) + " has no live plugin");
        }

        for (auto plugin : site.plugins->getPlugins())
        {
            const auto id = getEffectId (*plugin);

            if (id.isEmpty())
                continue;   // the instrument, the fader, the meter

            const auto known = std::any_of (site.effects.begin(), site.effects.end(),
                                            [&id] (const model::Effect& e) { return e.getId() == id; });

            if (! known)
                problems.push_back ("live plugin " + plugin->getPluginType() + " (" + id + ") in "
                                     + describe (site) + " is in no chain the song has");
        }
    }

    // The mapping every one of those sites was built on.
    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (edit);

    for (int i = 0; i < (int) generators.size(); ++i)
    {
        if (i >= tracks.size())
        {
            problems.push_back ("generator " + generators[(size_t) i].getName() + " has no track");
            continue;
        }

        if (getReturnTrackId (*tracks[i]).isNotEmpty())
            problems.push_back ("track " + juce::String (i) + " belongs to a return bus, but generator "
                                 + generators[(size_t) i].getName() + " expects to be there");
    }

    for (const auto& bus : song.getReturns())
    {
        const auto found = std::any_of (tracks.begin(), tracks.end(),
                                        [&bus] (te::AudioTrack* t) { return getReturnTrackId (*t) == bus.getId(); });

        if (! found)
            problems.push_back ("return bus " + bus.getName() + " has no track");
    }

    return problems;
}

te::Plugin* findEffectPlugin (const model::Song& song, te::Edit& edit,
                              const juce::String& ownerId, const juce::String& effectId)
{
    if (auto site = findEffectChainSite (song, edit, ownerId))
        return site->findPlugin (effectId);

    return nullptr;
}

} // namespace carve::sync
