#pragma once

#include <optional>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::sync
{

// Where an insert chain can live, and everything anyone needs to work on one.
//
// A song has three kinds of chain -- a generator's, a return bus's, and the
// master's -- and they differ only in which plugin list they are and what they
// sit after. That difference used to be spelled out separately everywhere the
// three had to be visited, and the bugs that came of it were all the same bug:
// somebody enumerated two of the three.
//
//   * saving walked returns and generators, so every knob on a master insert
//     came back at its default
//   * the mixer's staleness check knew generators and the master, so a return
//     bus's effect editor was closed by the timer the moment it opened
//
// Both were one forgotten loop. So there is one enumeration now, and anything
// that has to visit every chain goes through it -- see getEffectChainSites.
struct EffectChainSite
{
    // The generator's or the return's id. Empty means the master, which is
    // the same convention the model's automation targets use.
    juce::String ownerId;

    // What the model says is in this chain, in signal order.
    std::vector<model::Effect> effects;

    // The live chain, or null when the track behind it has not been built yet
    // (a generator added this tick, before the next sync).
    te::PluginList* plugins = nullptr;

    // What a plugin built for this chain is parented to, and where in the list
    // an insert belongs: after a generator's instrument, after a return's aux
    // return, at the top of the master's.
    juce::ValueTree ownerState;
    int insertAt = 0;

    // The live plugin EditSync stamped with this effect's id, or null.
    te::Plugin* findPlugin (const juce::String& effectId) const;
};

// Every chain in the song, in the order the mixer lays them out: the
// generators, then the return busses, then the master.
std::vector<EffectChainSite> getEffectChainSites (const model::Song&, te::Edit&);

// The site one owner id names, or nothing if the song has no such owner.
std::optional<EffectChainSite> findEffectChainSite (const model::Song&, te::Edit&,
                                                    const juce::String& ownerId);

// The live plugin behind one model effect, wherever its chain lives. This is
// the lookup the mixer needs and the one it must not reinvent.
te::Plugin* findEffectPlugin (const model::Song&, te::Edit&,
                              const juce::String& ownerId, const juce::String& effectId);

// What the sync promises about the Edit it has just built, checked: one line
// per thing found wrong, empty when the Edit and the song agree.
//
// Written as a list rather than an assertion so it has two users -- a debug
// build asserts on it after every sync, which turns ordinary use of the app
// into a search for these, and the property tests assert on it after every
// generated edit. Both of the bugs this file exists because of would have been
// caught here.
//
// It does not check what the model cannot promise: an external plugin the
// machine has never scanned legitimately has no live counterpart.
std::vector<juce::String> findSyncProblems (const model::Song&, te::Edit&);

// The three builders, for the sync itself: it visits a chain at the moment it
// has just finished building what the chain sits after, so it cannot use the
// whole-song enumeration above -- a generator's insert position is only known
// once its instrument is there.
EffectChainSite generatorChainSite (const model::Generator&, te::AudioTrack&);
EffectChainSite returnChainSite (const model::Return&, te::AudioTrack&);
EffectChainSite masterChainSite (const model::MasterBus&, te::Edit&);

} // namespace carve::sync
