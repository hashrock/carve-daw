#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

#include <juce_dsp/juce_dsp.h>

namespace carve::plugins::fx
{

// The output stages of the recorded instruments: a bus compressor for the
// drum kit, an amp (drive and a three-band EQ) for the bass. Each is off by
// default, so an instrument as it comes still plays the recording as it was.
//
// The static functions are the same maths the audio path uses, for the
// editors to draw their curves from: what a graph shows is what is applied.

//==============================================================================
// What a stage has been doing lately, for an editor to draw as it scrolls:
// one entry per audio block, written on the audio thread and read on the
// message thread. A torn read is a pixel drawn a block late, so no lock.
class LevelHistory
{
public:
    static constexpr int size = 512;

    LevelHistory()
    {
        // Silence until something plays, not 0 dB.
        for (auto& v : inputs)
            v.store (-100.0f, std::memory_order_relaxed);
    }

    struct Entry { float inputDb = -100.0f, reductionDb = 0.0f; };

    void push (float inputDb, float reductionDb) noexcept
    {
        const auto i = (writeIndex.load (std::memory_order_relaxed) + 1) % size;
        inputs[(size_t) i].store (inputDb, std::memory_order_relaxed);
        reductions[(size_t) i].store (reductionDb, std::memory_order_relaxed);
        writeIndex.store (i, std::memory_order_release);
    }

    // ago 0 is the newest.
    Entry get (int ago) const noexcept
    {
        const auto i = ((writeIndex.load (std::memory_order_acquire) - ago) % size + size) % size;
        return { inputs[(size_t) i].load (std::memory_order_relaxed), reductions[(size_t) i].load (std::memory_order_relaxed) };
    }

    int getWriteIndex() const noexcept  { return writeIndex.load (std::memory_order_acquire); }

private:
    std::array<std::atomic<float>, size> inputs {}, reductions {};
    std::atomic<int> writeIndex { 0 };
};

//==============================================================================
// A feed-forward compressor with a soft knee, mono, with a dry/wet mix for
// parallel compression -- the usual way to squash a kit and keep its hits.
class BusCompressor
{
public:
    struct Settings
    {
        float thresholdDb = -18.0f, ratio = 4.0f, attackMs = 10.0f, releaseMs = 120.0f, makeupDb = 0.0f, mix = 1.0f;
    };

    static constexpr float kneeDb = 6.0f;

    // The gain computer: output level for an input level, in dB.
    static float outputDb (float inputDb, float thresholdDb, float ratio) noexcept
    {
        const auto over = inputDb - thresholdDb;

        if (2.0f * over < -kneeDb)
            return inputDb;

        if (2.0f * over > kneeDb)
            return thresholdDb + over / ratio;

        const auto x = over + kneeDb / 2.0f;
        return inputDb + (1.0f / ratio - 1.0f) * x * x / (2.0f * kneeDb);
    }

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        envelopeDb = 0.0f;
    }

    void process (float* samples, int numSamples, const Settings& s, LevelHistory* history)
    {
        float* channels[] { samples };
        process (channels, 1, numSamples, s, history);
    }

    // Any number of channels, linked: the loudest of them drives one gain
    // for all, so a stereo image does not lean when one side peaks.
    void process (float* const* channels, int numChannels, int numSamples, const Settings& s, LevelHistory* history)
    {
        const auto attack = std::exp (-1.0f / (float) (juce::jmax (0.05f, s.attackMs) * 0.001 * fs));
        const auto release = std::exp (-1.0f / (float) (juce::jmax (1.0f, s.releaseMs) * 0.001 * fs));
        const auto makeup = juce::Decibels::decibelsToGain (s.makeupDb);
        float peakIn = 0.0f, mostReduction = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            float level = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                level = juce::jmax (level, std::abs (channels[ch][i]));

            peakIn = juce::jmax (peakIn, level);

            const auto inDb = juce::Decibels::gainToDecibels (level, -120.0f);
            const auto wanted = inDb - outputDb (inDb, s.thresholdDb, s.ratio);   // dB of reduction wanted

            // Attack while the reduction grows, release while it shrinks.
            const auto coefficient = wanted > envelopeDb ? attack : release;
            envelopeDb = wanted + coefficient * (envelopeDb - wanted);
            mostReduction = juce::jmax (mostReduction, envelopeDb);

            const auto gain = juce::Decibels::decibelsToGain (-envelopeDb) * makeup;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto x = channels[ch][i];
                channels[ch][i] = x + s.mix * (x * gain - x);
            }
        }

        lastReductionDb.store (mostReduction, std::memory_order_relaxed);

        if (history != nullptr)
            history->push (juce::Decibels::gainToDecibels (peakIn, -100.0f), mostReduction);
    }

    float getReductionDb() const noexcept  { return lastReductionDb.load (std::memory_order_relaxed); }

private:
    double fs = 44100.0;
    float envelopeDb = 0.0f;
    std::atomic<float> lastReductionDb { 0.0f };
};

//==============================================================================
// A bass amp: drive into a three-band tone stack, then a level. The drive is
// an asymmetric tanh run at twice the rate (a bass's low notes clipped make
// harmonics far up), with a blend back to the clean signal, which is how
// bass drive is usually played: grit on top, the fundamental left alone.
class BassAmp
{
public:
    struct Settings
    {
        float drive = 0.0f;       // 0..1
        float blend = 1.0f;       // 0 clean .. 1 all driven
        float bassDb = 0.0f, midDb = 0.0f, midHz = 700.0f, trebleDb = 0.0f;
        float levelDb = 0.0f;
    };

    static constexpr double bassHz = 90.0, trebleHz = 2800.0, midQ = 0.8, shelfQ = 0.707;

    // The drive's gain into the shaper: none at 0, +36 dB at 1.
    static float driveGain (float drive) noexcept  { return juce::Decibels::decibelsToGain (36.0f * drive); }

    // The shaper, normalised so a signal at the reference level comes out at
    // that level whatever the drive: turning it up adds grit, not volume.
    static float shape (float x, float drive) noexcept
    {
        const auto g = driveGain (drive);
        constexpr float bias = 0.2f, reference = 0.3f;
        const auto offset = std::tanh (bias);
        const auto norm = reference / (std::tanh (g * reference + bias) - offset);

        // Clipping also lifts everything under the reference -- a decaying
        // note stays loud -- so the drive gives back up to 9 dB as it rises.
        const auto compensation = juce::Decibels::decibelsToGain (-9.0f * drive);
        return (std::tanh (g * x + bias) - offset) * norm * compensation;
    }

    // RBJ biquads, as {b0, b1, b2, a1, a2} over a0.
    using Coefficients = std::array<double, 5>;

    static Coefficients lowShelf (double fs, double hz, double db)   { return shelf (fs, hz, db, true); }
    static Coefficients highShelf (double fs, double hz, double db)  { return shelf (fs, hz, db, false); }

    static Coefficients peak (double fs, double hz, double db)
    {
        const auto A = std::pow (10.0, db / 40.0), w0 = juce::MathConstants<double>::twoPi * hz / fs;
        const auto alpha = std::sin (w0) / (2.0 * midQ), cw = std::cos (w0);
        const auto a0 = 1.0 + alpha / A;
        return { (1.0 + alpha * A) / a0, -2.0 * cw / a0, (1.0 - alpha * A) / a0, -2.0 * cw / a0, (1.0 - alpha / A) / a0 };
    }

    static double magnitudeDb (const Coefficients& c, double hz, double fs)
    {
        const auto w = juce::MathConstants<double>::twoPi * hz / fs;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
        const auto h = (c[0] + c[1] * z1 + c[2] * z2) / (1.0 + c[3] * z1 + c[4] * z2);
        return 20.0 * std::log10 (std::abs (h) + 1.0e-12);
    }

    // The tone stack's response at a frequency, for the editor's curve.
    static double responseDb (const Settings& s, double hz, double fs)
    {
        return magnitudeDb (lowShelf (fs, bassHz, s.bassDb), hz, fs)
             + magnitudeDb (peak (fs, s.midHz, s.midDb), hz, fs)
             + magnitudeDb (highShelf (fs, trebleHz, s.trebleDb), hz, fs);
    }

    void prepare (double sampleRate, int maxBlock)
    {
        fs = sampleRate;
        blockLimit = juce::jmax (32, maxBlock);
        oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
            1, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        oversampling->initProcessing ((size_t) blockLimit);
        dry.assign ((size_t) blockLimit, 0.0f);

        for (auto& f : filters)
            f = {};

        lastSettings = {};
        lastSettings.midHz = -1.0f;   // forces the first coefficients
        level.reset (sampleRate, 0.03);
        driveSmoothed.reset (sampleRate, 0.03);
        level.setCurrentAndTargetValue (1.0f);
        driveSmoothed.setCurrentAndTargetValue (0.0f);
    }

    void process (float* samples, int numSamples, const Settings& s)
    {
        if (oversampling == nullptr)
            return;

        updateFilters (s);
        level.setTargetValue (juce::Decibels::decibelsToGain (s.levelDb));
        driveSmoothed.setTargetValue (s.drive);

        for (int done = 0; done < numSamples;)
        {
            const int n = juce::jmin (numSamples - done, blockLimit);
            auto* x = samples + done;

            // Drive, at twice the rate, with the clean signal blended back.
            if (s.drive > 0.0f || driveSmoothed.isSmoothing())
            {
                std::copy (x, x + n, dry.begin());

                float* channels[] { x };
                juce::dsp::AudioBlock<float> block (channels, 1, (size_t) n);
                auto up = oversampling->processSamplesUp (block);
                auto* u = up.getChannelPointer (0);
                const auto upCount = (int) up.getNumSamples();

                // The drive moves once per pair of oversampled samples.
                for (int i = 0; i < upCount; i += 2)
                {
                    const auto d = driveSmoothed.getNextValue();
                    u[i] = shape (u[i], d);
                    if (i + 1 < upCount)
                        u[i + 1] = shape (u[i + 1], d);
                }

                oversampling->processSamplesDown (block);

                for (int i = 0; i < n; ++i)
                    x[i] = dry[(size_t) i] + s.blend * (x[i] - dry[(size_t) i]);
            }

            // The tone stack, then the level.
            for (auto& f : filters)
                f.process (x, n);

            for (int i = 0; i < n; ++i)
                x[i] *= level.getNextValue();

            done += n;
        }
    }

private:
    static Coefficients shelf (double fs, double hz, double db, bool low)
    {
        const auto A = std::pow (10.0, db / 40.0), w0 = juce::MathConstants<double>::twoPi * hz / fs;
        const auto cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * shelfQ), sq = 2.0 * std::sqrt (A) * alpha;
        const auto s = low ? -1.0 : 1.0;   // the sign that turns the low shelf's formula into the high one's

        const auto b0 = A * ((A + 1.0) + s * (A - 1.0) * cw + sq);
        const auto b1 = -2.0 * s * A * ((A - 1.0) + s * (A + 1.0) * cw);
        const auto b2 = A * ((A + 1.0) + s * (A - 1.0) * cw - sq);
        const auto a0 = (A + 1.0) - s * (A - 1.0) * cw + sq;
        const auto a1 = 2.0 * s * ((A - 1.0) - s * (A + 1.0) * cw);
        const auto a2 = (A + 1.0) - s * (A - 1.0) * cw - sq;
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    struct Biquad
    {
        Coefficients c { 1.0, 0.0, 0.0, 0.0, 0.0 };
        double z1 = 0.0, z2 = 0.0;

        void process (float* x, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                const double in = x[i];
                const double out = c[0] * in + z1;
                z1 = c[1] * in - c[3] * out + z2;
                z2 = c[2] * in - c[4] * out;
                x[i] = (float) out;
            }
        }
    };

    void updateFilters (const Settings& s)
    {
        const auto same = [] (float a, float b) { return std::abs (a - b) < 1.0e-6f; };

        if (same (s.bassDb, lastSettings.bassDb) && same (s.midDb, lastSettings.midDb)
             && same (s.midHz, lastSettings.midHz) && same (s.trebleDb, lastSettings.trebleDb))
            return;

        filters[0].c = lowShelf (fs, bassHz, s.bassDb);
        filters[1].c = peak (fs, s.midHz, s.midDb);
        filters[2].c = highShelf (fs, trebleHz, s.trebleDb);
        lastSettings = s;
    }

    double fs = 44100.0;
    int blockLimit = 512;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    std::vector<float> dry;
    std::array<Biquad, 3> filters;
    Settings lastSettings;
    juce::SmoothedValue<float> level, driveSmoothed;
};

} // namespace carve::plugins::fx
