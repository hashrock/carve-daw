#pragma once

#include <juce_data_structures/juce_data_structures.h>

// ValueTree schema:
//
// SONG {name, tempo, loopStart, loopEnd}
// ├─ GENERATORS
// │  └─ GENERATOR {id, name, type, volumeDb, pan, mute, solo}
// │     │                                  type: "internal-synth" | "plugin"
// │     │                                  mixer properties are optional; see
// │     │                                  Generator for their defaults
// │     ├─ PLUGIN {desc, state}             desc: PluginDescription XML, state: base64 blob
// │     └─ PATTERNS
// │        └─ PATTERN {id, name, lengthBeats, slot}
// │           │        slot: optional pattern-slot key, "A1".."D9". Patterns
// │           │        without one (older songs, extras) are still listed.
// │           │        Unused slots are absent from the tree entirely.
// │           └─ NOTE {start, length, pitch, velocity}   times in beats (quarter notes)
// ├─ PLAYLIST
// │  └─ CLIP {generatorId, patternId, start}
// └─ AUTOMATION (M6)

namespace orionish::model::ids
{

#define ORIONISH_DECLARE_ID(name) inline const juce::Identifier name { #name };

ORIONISH_DECLARE_ID (SONG)
ORIONISH_DECLARE_ID (GENERATORS)
ORIONISH_DECLARE_ID (GENERATOR)
ORIONISH_DECLARE_ID (PATTERNS)
ORIONISH_DECLARE_ID (PATTERN)
ORIONISH_DECLARE_ID (NOTE)
ORIONISH_DECLARE_ID (PLUGIN)
ORIONISH_DECLARE_ID (PLAYLIST)
ORIONISH_DECLARE_ID (CLIP)
ORIONISH_DECLARE_ID (AUTOMATION)

ORIONISH_DECLARE_ID (id)
ORIONISH_DECLARE_ID (name)
ORIONISH_DECLARE_ID (tempo)
ORIONISH_DECLARE_ID (type)
ORIONISH_DECLARE_ID (lengthBeats)
ORIONISH_DECLARE_ID (slot)
ORIONISH_DECLARE_ID (start)
ORIONISH_DECLARE_ID (length)
ORIONISH_DECLARE_ID (pitch)
ORIONISH_DECLARE_ID (velocity)
ORIONISH_DECLARE_ID (generatorId)
ORIONISH_DECLARE_ID (patternId)
ORIONISH_DECLARE_ID (desc)
ORIONISH_DECLARE_ID (state)
ORIONISH_DECLARE_ID (loopStart)
ORIONISH_DECLARE_ID (loopEnd)
ORIONISH_DECLARE_ID (volumeDb)
ORIONISH_DECLARE_ID (pan)
ORIONISH_DECLARE_ID (mute)
ORIONISH_DECLARE_ID (solo)

#undef ORIONISH_DECLARE_ID

} // namespace orionish::model::ids
