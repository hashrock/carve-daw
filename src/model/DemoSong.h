#pragma once

#include "SongModel.h"

namespace orionish::model
{

// Builds a small built-in demo song (three internal-synth generators,
// 16 beats at 120 BPM) used by the CLI --demo option and the app shell.
Song buildDemoSong();

} // namespace orionish::model
