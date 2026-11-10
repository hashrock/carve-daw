#include "EffectChains.h"

#include <algorithm>
#include <limits>

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

EffectChainSite groupChainSite (const model::Group& group, te::AudioTrack& track)
{
    // At the head: a group track has no instrument and no aux return in front
    // of its chain, only the fader behind it.
    EffectChainSite site;
    site.ownerId = group.getId();
    site.effects = group.getEffects();
    site.plugins = &track.pluginList;
    site.ownerState = track.state;
    site.insertAt = 0;
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
    // At the head, ahead of the mastering chain. (The master volume is not in
    // this list at all: tracktion applies it after everything in it.)
    EffectChainSite site;
    site.effects = bus.getEffects();
    site.plugins = &edit.getMasterPluginList();
    site.ownerState = edit.getMasterPluginList().state;
    site.insertAt = 0;
    return site;
}

EffectChainSite masteringChainSite (const model::MasteringChain& chain, te::Edit& edit)
{
    // The same list as the master's, appended to: the master's effects go in
    // at the head, so this chain stays behind them for good -- the sync only
    // ever reorders a chain's plugins among the places they already hold.
    EffectChainSite site;
    site.ownerId = model::MasteringChain::ownerId;
    site.effects = chain.getEffects();
    site.plugins = &edit.getMasterPluginList();
    site.ownerState = edit.getMasterPluginList().state;
    site.insertAt = -1;
    site.active = isMasteringActive (edit);
    return site;
}

namespace
{
    const juce::Identifier masteringActiveProperty ("carveMasteringActive");
}

void setMasteringActive (te::Edit& edit, bool active)
{
    edit.state.setProperty (masteringActiveProperty, active, nullptr);
}

bool isMasteringActive (const te::Edit& edit)
{
    return edit.state.getProperty (masteringActiveProperty, true);
}

//==============================================================================
std::vector<EffectChainSite> getEffectChainSites (const model::Song& song, te::Edit& edit)
{
    std::vector<EffectChainSite> sites;

    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (edit);

    // Generator order == generator-track order, and the bus tracks (groups,
    // then returns) are appended after them: the same invariant the sync
    // itself runs on.
    juce::Array<te::AudioTrack*> generatorTracks;

    for (auto track : tracks)
        if (! isBusTrack (*track))
            generatorTracks.add (track);

    for (int i = 0; i < (int) generators.size(); ++i)
    {
        if (i < generatorTracks.size())
        {
            sites.push_back (generatorChainSite (generators[(size_t) i], *generatorTracks[i]));
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

    for (const auto& group : song.getGroups())
    {
        te::AudioTrack* groupTrack = nullptr;

        for (auto track : tracks)
            if (getGroupTrackId (*track) == group.getId())
                groupTrack = track;

        if (groupTrack != nullptr)
        {
            sites.push_back (groupChainSite (group, *groupTrack));
        }
        else
        {
            EffectChainSite site;
            site.ownerId = group.getId();
            site.effects = group.getEffects();
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
    sites.push_back (masteringChainSite (song.getMasteringChain(), edit));

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
        if (site.ownerId.isEmpty())
            return juce::String ("the master chain");

        if (site.ownerId == model::MasteringChain::ownerId)
            return juce::String ("the mastering chain");

        return "chain of owner " + site.ownerId;
    };

    const auto sites = getEffectChainSites (song, edit);

    for (auto& site : sites)
    {
        // A generator added this tick has no track until the next sync, which
        // is a state the app passes through rather than one it rests in.
        if (site.plugins == nullptr)
            continue;

        for (const auto& effect : site.effects)
        {
            if (effect.isExternal())
                continue;   // may be a plugin this machine does not have

            if (auto plugin = site.findPlugin (effect.getId()))
            {
                if (plugin->isEnabled() != (effect.isEnabled() && site.active))
                    problems.push_back ("effect " + effect.getType() + " (" + effect.getId()
                                         + ") in " + describe (site) + " is "
                                         + (plugin->isEnabled() ? "enabled" : "bypassed")
                                         + " but should not be");
            }
            else
            {
                problems.push_back ("effect " + effect.getType() + " (" + effect.getId()
                                     + ") in " + describe (site) + " has no live plugin");
            }
        }

        // The live plugins in the model's order. Only the effects that are
        // there are compared: a missing one is reported above.
        {
            juce::StringArray wanted, live;

            for (const auto& effect : site.effects)
                if (site.findPlugin (effect.getId()) != nullptr)
                    wanted.add (effect.getId());

            for (auto plugin : site.plugins->getPlugins())
                if (wanted.contains (getEffectId (*plugin)))
                    live.add (getEffectId (*plugin));

            if (wanted != live)
                problems.push_back ("the plugins of " + describe (site) + " are not in the song's order");
        }

        for (auto plugin : site.plugins->getPlugins())
        {
            const auto id = getEffectId (*plugin);

            if (id.isEmpty())
                continue;   // the instrument, the fader, the meter

            // Any chain that shares this list may own it: the master and
            // the mastering chain are one plugin list between them.
            const auto known = std::any_of (sites.begin(), sites.end(), [&] (const EffectChainSite& other)
            {
                return other.plugins == site.plugins
                        && std::any_of (other.effects.begin(), other.effects.end(),
                                        [&id] (const model::Effect& e) { return e.getId() == id; });
            });

            if (! known)
                problems.push_back ("live plugin " + plugin->getPluginType() + " (" + id + ") in "
                                     + describe (site) + " is in no chain the song has");
        }
    }

    // The mastering chain comes after the master's own effects: it is what
    // the finished mix goes through, not part of making it.
    {
        const auto master = masterChainSite (song.getMasterBus(), edit);
        const auto mastering = masteringChainSite (song.getMasteringChain(), edit);
        int lastMaster = -1, firstMastering = std::numeric_limits<int>::max();

        for (const auto& effect : master.effects)
            if (auto plugin = master.findPlugin (effect.getId()))
                lastMaster = juce::jmax (lastMaster, master.plugins->indexOf (plugin));

        for (const auto& effect : mastering.effects)
            if (auto plugin = mastering.findPlugin (effect.getId()))
                firstMastering = juce::jmin (firstMastering, mastering.plugins->indexOf (plugin));

        if (lastMaster > firstMastering)
            problems.push_back ("a mastering effect sits in front of a master effect");
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

        if (isBusTrack (*tracks[i]))
            problems.push_back ("track " + juce::String (i) + " belongs to a bus, but generator "
                                 + generators[(size_t) i].getName() + " expects to be there");
    }

    for (const auto& group : song.getGroups())
    {
        const auto found = std::any_of (tracks.begin(), tracks.end(),
                                        [&group] (te::AudioTrack* t) { return getGroupTrackId (*t) == group.getId(); });

        if (! found)
            problems.push_back ("group bus " + group.getName() + " has no track");
    }

    for (const auto& bus : song.getReturns())
    {
        const auto found = std::any_of (tracks.begin(), tracks.end(),
                                        [&bus] (te::AudioTrack* t) { return getReturnTrackId (*t) == bus.getId(); });

        if (! found)
            problems.push_back ("return bus " + bus.getName() + " has no track");
    }

    // Routing: a generator in a group must be feeding that group's track, and
    // one in no group must be feeding the master. This is the half of a group
    // that is not a chain, and nothing else checks it.
    for (const auto& generator : generators)
    {
        const auto groupId = generator.getGroupId();

        if (groupId.isEmpty())
            continue;

        te::AudioTrack* generatorTrack = nullptr;
        te::AudioTrack* groupTrack = nullptr;
        int index = 0;

        for (auto track : tracks)
        {
            if (getGroupTrackId (*track) == groupId)
                groupTrack = track;

            if (! isBusTrack (*track))
            {
                if (index < (int) generators.size() && generators[(size_t) index].getId() == generator.getId())
                    generatorTrack = track;

                ++index;
            }
        }

        if (generatorTrack == nullptr || groupTrack == nullptr)
            continue;   // already reported above

        if (generatorTrack->getOutput().getDestinationTrack() != groupTrack)
            problems.push_back ("generator " + generator.getName() + " is in a group but does not feed it");
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
