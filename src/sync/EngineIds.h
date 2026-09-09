#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "plugins/DrumSynthPlugin.h"
#include "plugins/NoteMonitorPlugin.h"

namespace te = tracktion;

namespace carve::sync
{

// Stamped by EditSync onto the tracktion plugin it builds for a model Effect.
// It is what lets a resync reconcile the chain instead of rebuilding it, and
// it is also how anything else tells an insert effect apart from the track's
// instrument -- an insert can be an ExternalPlugin too, so "is it external?"
// is not the question.
inline const juce::Identifier effectIdProperty ("carveEffectId");

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
                  || dynamic_cast<plugins::DrumSynthPlugin*> (plugin) != nullptr
                  || dynamic_cast<te::SamplerPlugin*> (plugin) != nullptr
                  || dynamic_cast<te::ExternalPlugin*> (plugin) != nullptr))
            return plugin;

    return nullptr;
}

// The note monitor EditSync keeps in front of a drum kit's sampler (see
// NoteMonitorPlugin.h), or null on a track that has none. The pad grid polls
// it to light the pads.
inline plugins::NoteMonitorPlugin* findNoteMonitorPlugin (te::AudioTrack& track)
{
    return track.pluginList.findFirstPluginOfType<plugins::NoteMonitorPlugin>();
}

// Stamped onto a return bus's track so track management can tell it from a
// generator track. Matching by stamp rather than by position is what lets the
// generator sync paths never touch a return track: an index shift must never
// point the generator machinery at a track full of shared reverb.
inline const juce::Identifier returnIdProperty ("carveReturnId");

inline juce::String getReturnTrackId (const te::Track& track)
{
    return track.state.getProperty (returnIdProperty).toString();
}

inline bool isReturnTrack (const te::Track& track)  { return getReturnTrackId (track).isNotEmpty(); }

} // namespace carve::sync
