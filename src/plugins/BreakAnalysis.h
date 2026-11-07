#pragma once

#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

namespace carve::plugins::breakanalysis
{

// Where the hits in a drum loop are, for the break slicer's Detect button.
//
// What is looked for is a sudden rise in level, measured on the signal's
// first difference rather than on the signal itself: the difference weighs
// the top end, where a stick hitting a skin is, and turns down the bass of
// the kick tail and the room that would otherwise smear every rise. Each hit
// becomes a cut a little before it starts, snapped to a zero crossing, so the
// slice opens on the attack instead of on a click.
//
// sensitivity is 0..1: at 0 only the loudest hits count, at 1 every ghost
// note does. The first cut is always at the start of the loop -- the slices
// cover the whole of it, and whatever comes before the first hit is part of
// the first slice. At most maxSlices cuts come back, the strongest hits kept
// when there are more.
std::vector<double> detectTransients (const juce::AudioBuffer<float>&, double sampleRate,
                                      double sensitivity, int maxSlices);

// The nearest point to `seconds` where the loop crosses zero, within
// windowSeconds of it either way (summed to mono); `seconds` itself when
// there is no crossing that close. A cut there starts its slice from silence
// rather than mid-wave, which is what keeps a chopped break from clicking.
double snapToZeroCrossing (const juce::AudioBuffer<float>&, double sampleRate,
                           double seconds, double windowSeconds = 0.004);

} // namespace carve::plugins::breakanalysis
