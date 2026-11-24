#pragma once

#include "SignalPathChecks.h"

namespace carve::test
{

// Plays every factory preset of every built-in instrument through a phrase
// that suits it (a groove for the kits, a line for the basses, chords and a
// tune for the keys, a sung line for the voice), renders each one to its own
// WAV in `folder`, and prints a table of what came out:
//
//   peak       dBFS of the loudest sample (0.0 means it clipped)
//   RMS        dBFS over the loudest 400 ms
//   low        how much of that is below 200 Hz, in dB against the whole
//              (how "thick" the sound is, the complaint the presets answer)
//   CPU        render time against the audio's length, single-threaded
//
// and the same as a Markdown table in folder/presets.md. Also checks that a
// preset, once applied, is recognised as itself (the preset bar's tick).
//
// `filter`, when not empty, is a substring of the instrument or preset name.
// Returns the number of presets that were silent, clipped, or not recognised.
// Run by carve-render --audition-presets.
int runPresetAudition (tracktion::Engine&, RenderSong render, const juce::File& folder, const juce::String& filter);

}
