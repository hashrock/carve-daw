#pragma once

#include <juce_data_structures/juce_data_structures.h>

// ValueTree schema:
//
// SONG {name, tempo, loopStart, loopEnd}
// ├─ MASTER {volumeDb}                      the mix bus: its fader and the
// │  └─ EFFECTS                            effects across the whole mix. Same
// │     └─ EFFECT {...}                    EFFECT nodes a generator carries.
// ├─ TEMPOS                                 changes after the first; the song's
// │  └─ TEMPO {start, bpm}                  own "tempo" is what plays until the
// │                                         first one. Absent = one tempo.
// ├─ TIMESIGS
// │  └─ TIMESIG {start, numerator, denominator}   absent = 4/4 throughout
// ├─ GENERATORS
// │  └─ GENERATOR {id, name, type, volumeDb, pan, mute, solo}
// │     │                          type: "internal-synth" | "plugin" | "sampler"
// │     │                                | "drum-sampler" | "audio"
// │     │                                  mixer properties are optional; see
// │     │                                  Generator for their defaults
// │     ├─ PLUGIN {desc, state}             desc: PluginDescription XML, state: base64 blob
// │     ├─ SOUNDS                           sampler types only, absent otherwise
// │     │  └─ SOUND {id, name, file, relPath, rootNote, minNote, maxNote, gainDb, pan}
// │     │        file: absolute path to the sample. relPath: the same file
// │     │        relative to the .carve's folder, rewritten on every save.
// │     │        Loading prefers relPath when it resolves to a file that
// │     │        exists, so a song moved with its samples plays elsewhere.
// │     │        Everything but the paths is optional; see SamplerSound for
// │     │        the defaults (whole keyboard, root C3, unity gain).
// │     ├─ EFFECTS
// │     │  └─ EFFECT {id, type, desc, state, enabled}
// │     │        type: a tracktion internal plugin's xmlTypeName, or "plugin"
// │     │        for an external one (then desc/state describe it)
// │     └─ PATTERNS
// │        └─ PATTERN {id, name, lengthBeats, slot}
// │           │        slot: optional pattern-slot key, "A1".."D9". Patterns
// │           │        without one (older songs, extras) are still listed.
// │           │        Unused slots are absent from the tree entirely.
// │           └─ NOTE {start, length, pitch, velocity}   times in beats (quarter notes)
// ├─ PLAYLIST
// │  ├─ CLIP {generatorId, patternId, start, length, transpose}
// │  │                               length and transpose are per-placement
// │  │                               and optional; see PlaylistClip
// │  └─ AUDIOCLIP {id, generatorId, file, relPath, start, length, offset}
// │                                  start is in beats; length and offset are in
// │                                  SECONDS -- see AudioClip for why
// │                                  a file placed on an "audio" generator's
// │                                  row. Its own node type rather than a CLIP
// │                                  with a file: it names no pattern, carries
// │                                  no transpose, and needs an explicit
// │                                  length, so every consumer of CLIP would
// │                                  have had to special-case it anyway.
// │                                  offset is how far into the source the
// │                                  placement begins, in seconds like its
// │                                  length. file/relPath work exactly as
// │                                  SOUND's do -- see FileRef.
// │                                  id is what lets EditSync match a live
// │                                  wave clip to its placement.
// └─ AUTOMATION (M6)

namespace carve::model::ids
{

#define CARVE_DECLARE_ID(name) inline const juce::Identifier name { #name };

CARVE_DECLARE_ID (SONG)
CARVE_DECLARE_ID (GENERATORS)
CARVE_DECLARE_ID (GENERATOR)
CARVE_DECLARE_ID (PATTERNS)
CARVE_DECLARE_ID (PATTERN)
CARVE_DECLARE_ID (NOTE)
CARVE_DECLARE_ID (PLUGIN)
CARVE_DECLARE_ID (SOUNDS)
CARVE_DECLARE_ID (SOUND)
CARVE_DECLARE_ID (EFFECTS)
CARVE_DECLARE_ID (EFFECT)
CARVE_DECLARE_ID (PLAYLIST)
CARVE_DECLARE_ID (CLIP)
CARVE_DECLARE_ID (AUDIOCLIP)
CARVE_DECLARE_ID (MASTER)
CARVE_DECLARE_ID (TEMPOS)
CARVE_DECLARE_ID (TEMPO)
CARVE_DECLARE_ID (TIMESIGS)
CARVE_DECLARE_ID (TIMESIG)
CARVE_DECLARE_ID (AUTOMATION)

CARVE_DECLARE_ID (id)
CARVE_DECLARE_ID (name)
CARVE_DECLARE_ID (tempo)
CARVE_DECLARE_ID (bpm)
CARVE_DECLARE_ID (numerator)
CARVE_DECLARE_ID (denominator)
CARVE_DECLARE_ID (type)
CARVE_DECLARE_ID (lengthBeats)
CARVE_DECLARE_ID (slot)
CARVE_DECLARE_ID (start)
CARVE_DECLARE_ID (length)
CARVE_DECLARE_ID (pitch)
CARVE_DECLARE_ID (velocity)
CARVE_DECLARE_ID (generatorId)
CARVE_DECLARE_ID (patternId)
CARVE_DECLARE_ID (desc)
CARVE_DECLARE_ID (state)
CARVE_DECLARE_ID (transpose)
CARVE_DECLARE_ID (enabled)
CARVE_DECLARE_ID (loopStart)
CARVE_DECLARE_ID (loopEnd)
CARVE_DECLARE_ID (volumeDb)
CARVE_DECLARE_ID (pan)
CARVE_DECLARE_ID (mute)
CARVE_DECLARE_ID (solo)
CARVE_DECLARE_ID (file)
CARVE_DECLARE_ID (relPath)
CARVE_DECLARE_ID (rootNote)
CARVE_DECLARE_ID (minNote)
CARVE_DECLARE_ID (maxNote)
CARVE_DECLARE_ID (gainDb)
CARVE_DECLARE_ID (offset)

#undef CARVE_DECLARE_ID

} // namespace carve::model::ids
