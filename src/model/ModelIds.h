#pragma once

#include <juce_data_structures/juce_data_structures.h>

// ValueTree schema:
//
// SONG {name, tempo}
// ├─ GENERATORS
// │  └─ GENERATOR {id, name, type}          type: "internal-synth" | "plugin"
// │     ├─ PLUGIN {desc, state}             desc: PluginDescription XML, state: base64 blob
// │     └─ PATTERNS
// │        └─ PATTERN {id, name, lengthBeats}
// │           └─ NOTE {start, length, pitch, velocity}   times in beats (quarter notes)
// ├─ PLAYLIST
// │  └─ CLIP {generatorId, patternId, start}
// ├─ MIXER      (M4)
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
ORIONISH_DECLARE_ID (MIXER)
ORIONISH_DECLARE_ID (AUTOMATION)

ORIONISH_DECLARE_ID (id)
ORIONISH_DECLARE_ID (name)
ORIONISH_DECLARE_ID (tempo)
ORIONISH_DECLARE_ID (type)
ORIONISH_DECLARE_ID (lengthBeats)
ORIONISH_DECLARE_ID (start)
ORIONISH_DECLARE_ID (length)
ORIONISH_DECLARE_ID (pitch)
ORIONISH_DECLARE_ID (velocity)
ORIONISH_DECLARE_ID (generatorId)
ORIONISH_DECLARE_ID (patternId)
ORIONISH_DECLARE_ID (desc)
ORIONISH_DECLARE_ID (state)

#undef ORIONISH_DECLARE_ID

} // namespace orionish::model::ids
