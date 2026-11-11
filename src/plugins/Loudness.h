#pragma once

#include <array>
#include <cmath>
#include <algorithm>

namespace carve::plugins
{

// Loudness as ITU-R BS.1770 measures it (LUFS): K-weighted -- a high shelf
// for the head and a high-pass for the bass the ear barely counts -- mean
// square over 400ms blocks, overlapping by 75%, gated at -70 LUFS and then
// 10 LU under the ungated result. What streaming services normalise to, and
// so the fair way to set a reference and a mix side by side.
//
// Feed it blocks as they come (any size), read integrated() whenever. Nothing
// is allocated after construction -- the blocks are kept as a histogram of
// their loudness in 0.1 LU steps, which is all the gating needs -- so it can
// run on the audio thread for as long as a song plays.
class LoudnessMeter
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        design();
        reset();
    }

    void reset()
    {
        for (auto& ch : state)
            ch = {};

        stepSamples = std::max (1, (int) std::lround (0.1 * fs));   // 100ms, a quarter block
        stepEnergy = 0.0;
        stepCount = 0;
        steps = {};
        stepsTaken = 0;
        binCount.fill (0);
        binEnergy.fill (0.0);
        shortCount.fill (0);
        shortEnergy.fill (0.0);
        momentaryNow = shortTermNow = -INFINITY;
    }

    // Interleaving is the caller's: one pointer per channel, up to two.
    void process (const float* const* channels, int numChannels, int numSamples)
    {
        numChannels = std::min (numChannels, 2);

        for (int i = 0; i < numSamples; ++i)
        {
            double sum = 0.0;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto y = kWeight (state[(size_t) ch], channels[ch][i]);
                sum += y * y;
            }

            // A mono signal counts as both channels, as it would reach both
            // speakers.
            stepEnergy += numChannels == 1 ? 2.0 * sum : sum;

            if (++stepCount == stepSamples)
            {
                steps[(size_t) (stepsTaken++ % numSteps)] = stepEnergy / stepSamples;
                stepEnergy = 0.0;
                stepCount = 0;

                // Momentary: a 400ms block every 100ms, the last four steps --
                // also what the integrated measure is made of. Short-term: the
                // last three seconds, which the loudness range is made of.
                const auto meanOfLast = [this] (int count)
                {
                    double total = 0.0;
                    for (int k = 0; k < count; ++k)
                        total += steps[(size_t) ((stepsTaken - 1 - k) % numSteps)];
                    return total / count;
                };

                if (stepsTaken >= 4)
                {
                    const auto block = meanOfLast (4);
                    momentaryNow = lufs (block);
                    addBlock (block);
                }

                if (stepsTaken >= numSteps)
                {
                    const auto block = meanOfLast (numSteps);
                    shortTermNow = lufs (block);
                    addShortTerm (block);
                }
            }
        }
    }

    // Gated integrated loudness so far, or -inf for nothing loud enough yet.
    double integrated() const
    {
        // Everything in the histogram is over the absolute gate already.
        double sum = 0.0;
        long long count = 0;

        for (size_t i = 0; i < numBins; ++i)
        {
            sum += binEnergy[i];
            count += binCount[i];
        }

        if (count == 0)
            return -INFINITY;

        const auto relativeGate = lufs (sum / (double) count) - 10.0;
        sum = 0.0;
        count = 0;

        for (size_t i = 0; i < numBins; ++i)
            if (binLufs (i) + binWidth > relativeGate)
            {
                sum += binEnergy[i];
                count += binCount[i];
            }

        return count > 0 ? lufs (sum / (double) count) : -INFINITY;
    }

    // How many 100ms steps have been measured: moves when momentary() and
    // shortTerm() have a new value.
    long long getStepsTaken() const  { return stepsTaken; }

    // The last 400ms and the last 3s, as they stand.
    double momentary() const  { return momentaryNow; }
    double shortTerm() const  { return shortTermNow; }

    // EBU Tech 3342: the spread of the short-term loudness, gated at -70 LUFS
    // and 20 LU under its own mean, from its 10th to its 95th percentile.
    double loudnessRange() const
    {
        double sum = 0.0;
        long long count = 0;

        for (size_t i = 0; i < numBins; ++i)
        {
            sum += shortEnergy[i];
            count += shortCount[i];
        }

        if (count == 0)
            return 0.0;

        const auto gate = lufs (sum / (double) count) - 20.0;
        long long kept = 0;

        for (size_t i = 0; i < numBins; ++i)
            if (binLufs (i) + binWidth > gate)
                kept += shortCount[i];

        if (kept < 2)
            return 0.0;

        auto percentile = [&] (double p)
        {
            const auto wanted = (long long) std::ceil (p * (double) kept);
            long long seen = 0;

            for (size_t i = 0; i < numBins; ++i)
                if (binLufs (i) + binWidth > gate && (seen += shortCount[i]) >= wanted)
                    return binLufs (i) + binWidth * 0.5;

            return binLufs (numBins - 1);
        };

        return percentile (0.95) - percentile (0.10);
    }

    bool hasMeasured() const
    {
        for (auto c : binCount)
            if (c > 0)
                return true;

        return false;
    }

    // One go over a whole buffer: what a file is loaded for.
    static double measure (const float* const* channels, int numChannels, int numSamples, double sampleRate)
    {
        LoudnessMeter meter;
        meter.prepare (sampleRate);
        meter.process (channels, numChannels, numSamples);
        return meter.integrated();
    }

private:
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct State { double x1 = 0, x2 = 0, y1 = 0, y2 = 0, u1 = 0, u2 = 0, z1 = 0, z2 = 0; };

    // The two K-weighting stages, designed for this rate from their analog
    // prototypes (the published 48kHz coefficients are this at 48kHz).
    void design()
    {
        constexpr double pi = 3.14159265358979323846;

        {
            const double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
            const double K = std::tan (pi * f0 / fs);
            const double Vh = std::pow (10.0, gain / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / q + K * K;
            shelf = { (Vh + Vb * K / q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / q + K * K) / a0,
                      2.0 * (K * K - 1.0) / a0, (1.0 - K / q + K * K) / a0 };
        }

        {
            const double f0 = 38.13547087602444, q = 0.5003270373238773;
            const double K = std::tan (pi * f0 / fs);
            const double a0 = 1.0 + K / q + K * K;
            highPass = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / q + K * K) / a0 };
        }
    }

    double kWeight (State& s, double x) const
    {
        const double u = shelf.b0 * x + shelf.b1 * s.x1 + shelf.b2 * s.x2 - shelf.a1 * s.u1 - shelf.a2 * s.u2;
        s.x2 = s.x1; s.x1 = x; s.u2 = s.u1; s.u1 = u;

        const double y = highPass.b0 * u + highPass.b1 * s.z1 + highPass.b2 * s.z2 - highPass.a1 * s.y1 - highPass.a2 * s.y2;
        s.z2 = s.z1; s.z1 = u; s.y2 = s.y1; s.y1 = y;
        return y;
    }

    static double lufs (double meanSquare)  { return -0.691 + 10.0 * std::log10 (meanSquare + 1.0e-30); }

    static constexpr double lowest = -70.0, binWidth = 0.1;
    static constexpr size_t numBins = 900;   // -70 .. +20 LUFS

    static double binLufs (size_t i)  { return lowest + binWidth * (double) i; }

    void addBlock (double meanSquare)
    {
        const auto l = lufs (meanSquare);

        if (l <= lowest)
            return;

        const auto bin = std::min (numBins - 1, (size_t) ((l - lowest) / binWidth));
        ++binCount[bin];
        binEnergy[bin] += meanSquare;
    }

    void addShortTerm (double meanSquare)
    {
        const auto l = lufs (meanSquare);

        if (l <= lowest)
            return;

        const auto bin = std::min (numBins - 1, (size_t) ((l - lowest) / binWidth));
        ++shortCount[bin];
        shortEnergy[bin] += meanSquare;
    }

    static constexpr int numSteps = 30;   // 3 seconds of 100ms steps

    double fs = 48000.0;
    Biquad shelf, highPass;
    std::array<State, 2> state {};
    int stepSamples = 4800, stepCount = 0;
    double stepEnergy = 0.0;
    std::array<double, numSteps> steps {};
    std::array<long long, numBins> shortCount {};
    std::array<double, numBins> shortEnergy {};
    double momentaryNow = -INFINITY, shortTermNow = -INFINITY;
    long long stepsTaken = 0;
    std::array<long long, numBins> binCount {};
    std::array<double, numBins> binEnergy {};
};

//==============================================================================
// True peak, as BS.1770 Annex 2 has it: the signal upsampled four times and
// its largest sample taken, so the peak between samples -- the one a DAC or
// a lossy encoder will make -- is seen. A 48-tap windowed-sinc interpolator,
// twelve taps a phase. Allocation-free once constructed.
class TruePeakMeter
{
public:
    TruePeakMeter()
    {
        constexpr double pi = 3.14159265358979323846;

        for (int phase = 0; phase < 4; ++phase)
            for (int t = 0; t < taps; ++t)
            {
                // Taps centred between samples 5 and 6 of the history.
                const double x = (double) (t - taps / 2 + 1) - phase / 4.0;
                const double sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (pi * x) / (pi * x);
                const double w = 0.5 * (1.0 + std::cos (pi * x / (taps / 2.0 + 0.5)));   // Hann
                coefficients[(size_t) phase][(size_t) t] = sinc * w;
            }

        reset();
    }

    void reset()
    {
        for (auto& h : history)
            h.fill (0.0f);

        position = 0;
        peak = 0.0;
    }

    void process (const float* const* channels, int numChannels, int numSamples)
    {
        numChannels = std::min (numChannels, 2);

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& h = history[(size_t) ch];
                h[(size_t) position] = channels[ch][i];

                for (int phase = 0; phase < 4; ++phase)
                {
                    double y = 0.0;
                    for (int t = 0; t < taps; ++t)
                        y += coefficients[(size_t) phase][(size_t) t] * h[(size_t) ((position - t + taps) % taps)];

                    peak = std::max (peak, std::abs (y));
                }
            }

            position = (position + 1) % taps;
        }
    }

    // Since the last reset, in dBTP.
    double getPeakDb() const  { return 20.0 * std::log10 (std::max (peak, 1.0e-10)); }

private:
    static constexpr int taps = 12;
    std::array<std::array<double, taps>, 4> coefficients {};
    std::array<std::array<float, taps>, 2> history {};
    int position = 0;
    double peak = 0.0;
};

} // namespace carve::plugins
