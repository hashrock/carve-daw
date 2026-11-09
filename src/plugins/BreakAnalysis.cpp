#include "BreakAnalysis.h"

#include <algorithm>
#include <cmath>

namespace carve::plugins::breakanalysis
{

namespace
{
    float monoAt (const juce::AudioBuffer<float>& buffer, int frame)
    {
        float sum = 0.0f;

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            sum += buffer.getSample (ch, frame);

        return sum;
    }
}

double snapToZeroCrossing (const juce::AudioBuffer<float>& buffer, double sampleRate,
                           double seconds, double windowSeconds)
{
    const int numFrames = buffer.getNumSamples();

    if (numFrames < 2 || sampleRate <= 0.0 || buffer.getNumChannels() == 0)
        return seconds;

    const int centre = juce::jlimit (0, numFrames - 1, (int) std::lround (seconds * sampleRate));
    const int window = juce::jmax (1, (int) std::lround (windowSeconds * sampleRate));

    // Outward from the centre, so the nearest crossing wins. A crossing
    // between frames f-1 and f is taken at f: the slice starts on the first
    // frame of the new half-wave.
    for (int distance = 0; distance <= window; ++distance)
    {
        for (int frame : { centre - distance, centre + distance })
        {
            if (frame < 1 || frame >= numFrames)
                continue;

            const auto before = monoAt (buffer, frame - 1);
            const auto here = monoAt (buffer, frame);

            if (juce::exactlyEqual (here, 0.0f) || (before < 0.0f) != (here < 0.0f))
                return frame / sampleRate;
        }
    }

    return seconds;
}

std::vector<double> detectTransients (const juce::AudioBuffer<float>& buffer, double sampleRate,
                                      double sensitivity, int maxSlices)
{
    std::vector<double> cuts { 0.0 };
    const int numFrames = buffer.getNumSamples();

    if (numFrames < 2 || sampleRate <= 0.0 || buffer.getNumChannels() == 0 || maxSlices <= 1)
        return cuts;

    // Level in 2.5 ms hops: fine enough to place a hit, coarse enough that a
    // single cycle of a low tom is not a rise of its own.
    const int hop = juce::jmax (16, (int) std::lround (sampleRate * 0.0025));
    const int numHops = numFrames / hop;

    std::vector<double> levelDb ((size_t) numHops, -120.0);

    for (int h = 0; h < numHops; ++h)
    {
        double energy = 0.0;

        for (int i = h * hop; i < (h + 1) * hop; ++i)
        {
            const auto difference = i > 0 ? monoAt (buffer, i) - monoAt (buffer, i - 1) : 0.0f;
            energy += (double) difference * difference;
        }

        levelDb[(size_t) h] = 10.0 * std::log10 (energy / hop + 1.0e-12);
    }

    const auto loudest = numHops > 0 ? *std::max_element (levelDb.begin(), levelDb.end()) : -120.0;

    // How far a hop has to rise over the ones just before it, and how loud
    // it has to be at all: both relax as the sensitivity goes up. The floor
    // keeps the tail of a crash from counting as a dozen hits of its own.
    const double riseDb = juce::jmap (juce::jlimit (0.0, 1.0, sensitivity), 10.0, 3.0);
    const double floorDb = loudest - juce::jmap (juce::jlimit (0.0, 1.0, sensitivity), 30.0, 54.0);

    // Two hits closer than this are one hit: a flam is a single slice.
    const int minGapHops = juce::jmax (1, (int) std::lround (0.045 * sampleRate / hop));
    constexpr int lookBack = 4;   // 10 ms of what came before

    struct Hit
    {
        int hop;
        double strength;
    };

    std::vector<Hit> hits;

    for (int h = lookBack; h < numHops; ++h)
    {
        double before = 0.0;

        for (int k = 1; k <= lookBack; ++k)
            before += levelDb[(size_t) (h - k)];

        before /= lookBack;
        const auto rise = levelDb[(size_t) h] - before;

        if (rise < riseDb || levelDb[(size_t) h] < floorDb)
            continue;

        // The steepest hop of a rise is the hit; the ones either side of it
        // are the same hit.
        if (! hits.empty() && h - hits.back().hop < minGapHops)
        {
            if (rise > hits.back().strength)
                hits.back() = { h, rise };

            continue;
        }

        hits.push_back ({ h, rise });
    }

    // Too many: the strongest, back in time order.
    if ((int) hits.size() > maxSlices - 1)
    {
        std::sort (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b) { return a.strength > b.strength; });
        hits.resize ((size_t) maxSlices - 1);
        std::sort (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b) { return a.hop < b.hop; });
    }

    for (const auto& hit : hits)
    {
        // The hop where the level started climbing, give or take: a cut on
        // the steepest hop would shave the very front of the attack off.
        const double seconds = juce::jmax (0.0, (hit.hop - 1) * hop / sampleRate);
        const double snapped = snapToZeroCrossing (buffer, sampleRate, seconds);

        // Too close to the start to be a slice of its own.
        if (snapped < 0.01)
            continue;

        if (snapped - cuts.back() >= 0.01)
            cuts.push_back (snapped);
    }

    return cuts;
}

} // namespace carve::plugins::breakanalysis
