#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace orionish::sync
{

// Stamped by EditSync onto the tracktion plugin it builds for a model Effect.
// It is what lets a resync reconcile the chain instead of rebuilding it, and
// it is also how anything else tells an insert effect apart from the track's
// instrument -- an insert can be an ExternalPlugin too, so "is it external?"
// is not the question.
inline const juce::Identifier effectIdProperty ("orionishEffectId");

inline juce::String getEffectId (const te::Plugin& plugin)
{
    return plugin.state.getProperty (effectIdProperty).toString();
}

inline bool isEffectPlugin (const te::Plugin& plugin)  { return getEffectId (plugin).isNotEmpty(); }

// The track's instrument: the first plugin that isn't one of our insert
// effects. Callers that reach for findFirstPluginOfType<ExternalPlugin> will
// pick up an external insert effect instead.
inline te::Plugin* findInstrumentPlugin (te::AudioTrack& track)
{
    for (auto plugin : track.pluginList.getPlugins())
        if (! isEffectPlugin (*plugin)
             && (dynamic_cast<te::FourOscPlugin*> (plugin) != nullptr
                  || dynamic_cast<te::SamplerPlugin*> (plugin) != nullptr
                  || dynamic_cast<te::ExternalPlugin*> (plugin) != nullptr))
            return plugin;

    return nullptr;
}

} // namespace orionish::sync
