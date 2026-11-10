#pragma once

#include <functional>

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace carve::test
{

// Renders a song to a WAV file at the given sample rate; true on success.
// carve-render passes in its own render, so what is measured is exactly what
// an export writes.
using RenderSong = std::function<bool (const model::Song&, const juce::File&, double sampleRate)>;

// Sends test signals through the mixer, the master bus, the resamplers and a
// handful of effects, renders each case, and compares the result with what
// it should be: gain to a hundredth of a dB, timing to the sample, distortion
// and band edges where they apply. Prints a table; returns the number of
// checks that failed. Run by carve-render --check-signal-path.
int runSignalPathChecks (RenderSong render);

}
