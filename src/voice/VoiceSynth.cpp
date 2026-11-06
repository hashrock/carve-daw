#include "VoiceSynth.h"

#include <algorithm>
#include <list>
#include <mutex>

#include <juce_audio_formats/juce_audio_formats.h>

#include <world/cheaptrick.h>
#include <world/d4c.h>
#include <world/dio.h>
#include <world/stonemask.h>
#include <world/synthesis.h>

namespace carve::voice
{

namespace
{
    constexpr double framePeriodMs = 5.0;

    // A sung note's range, a little wider than any voice: WORLD's defaults
    // (71..800 Hz) clip a high soprano's head voice.
    constexpr double f0FloorHz = 70.0;
    constexpr double f0CeilHz = 1100.0;

    // Reads a stretch of a WAV file as mono doubles: [startMs, endMs), with
    // endMs <= 0 meaning the end of the file. Empty on failure.
    std::vector<double> readRegion (const juce::File& file, double startMs, double endMs, int& sampleRate,
                                    double& fileLengthMs, juce::String& error)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (file.createInputStream().release(), true));

        if (reader == nullptr)
        {
            error = "Cannot read " + file.getFileName();
            return {};
        }

        sampleRate = (int) reader->sampleRate;
        fileLengthMs = 1000.0 * (double) reader->lengthInSamples / reader->sampleRate;

        const auto start = juce::jlimit ((juce::int64) 0, reader->lengthInSamples,
                                         (juce::int64) std::llround (startMs * 0.001 * reader->sampleRate));
        const auto end = endMs <= 0.0 ? reader->lengthInSamples
                                      : juce::jlimit (start, reader->lengthInSamples,
                                                      (juce::int64) std::llround (endMs * 0.001 * reader->sampleRate));
        const auto length = (int) (end - start);

        if (length <= 0)
        {
            error = "Empty region in " + file.getFileName();
            return {};
        }

        juce::AudioBuffer<float> buffer ((int) reader->numChannels, length);
        reader->read (&buffer, 0, length, start, true, true);

        std::vector<double> mono ((size_t) length, 0.0);
        const auto scale = 1.0 / juce::jmax (1, buffer.getNumChannels());

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* data = buffer.getReadPointer (ch);

            for (int i = 0; i < length; ++i)
                mono[(size_t) i] += data[i] * scale;
        }

        return mono;
    }

    // Where an entry's sound ends, in ms from the start of the file.
    double cutoffEndMs (const OtoEntry& entry, double fileLengthMs)
    {
        if (entry.cutoffMs < 0.0)
            return entry.offsetMs - entry.cutoffMs;

        return fileLengthMs - entry.cutoffMs;
    }

    double fileLengthMsOf (const juce::File& file)
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (file.createInputStream().release(), true));
        return reader != nullptr ? 1000.0 * (double) reader->lengthInSamples / reader->sampleRate : 0.0;
    }

    //==========================================================================
    // WORLD analysis of one stretch of a recording: the pitch, the spectral
    // envelope and the aperiodicity, every 5 ms.
    struct Analysis
    {
        int fs = 44100;
        int fftSize = 2048;
        std::vector<double> f0;
        std::vector<std::vector<double>> spectrum, aperiodicity;

        int getNumFrames() const  { return (int) f0.size(); }
    };

    std::vector<double> estimateF0 (const std::vector<double>& x, int fs, std::vector<double>& times)
    {
        DioOption option;
        InitializeDioOption (&option);
        option.frame_period = framePeriodMs;
        option.f0_floor = f0FloorHz;
        option.f0_ceil = f0CeilHz;
        option.speed = 1;

        const auto frames = GetSamplesForDIO (fs, (int) x.size(), framePeriodMs);
        times.assign ((size_t) frames, 0.0);
        std::vector<double> raw ((size_t) frames, 0.0), refined ((size_t) frames, 0.0);

        Dio (x.data(), (int) x.size(), fs, &option, times.data(), raw.data());
        StoneMask (x.data(), (int) x.size(), fs, times.data(), raw.data(), frames, refined.data());
        return refined;
    }

    std::shared_ptr<const Analysis> analyse (const std::vector<double>& x, int fs)
    {
        auto analysis = std::make_shared<Analysis>();
        analysis->fs = fs;

        std::vector<double> times;
        analysis->f0 = estimateF0 (x, fs, times);
        const auto frames = analysis->getNumFrames();

        CheapTrickOption cheapTrick;
        InitializeCheapTrickOption (fs, &cheapTrick);
        cheapTrick.f0_floor = f0FloorHz;
        cheapTrick.fft_size = GetFFTSizeForCheapTrick (fs, &cheapTrick);
        analysis->fftSize = cheapTrick.fft_size;

        const auto bins = (size_t) analysis->fftSize / 2 + 1;
        analysis->spectrum.assign ((size_t) frames, std::vector<double> (bins, 0.0));
        analysis->aperiodicity.assign ((size_t) frames, std::vector<double> (bins, 0.0));

        std::vector<double*> spectrumRows, aperiodicityRows;

        for (int i = 0; i < frames; ++i)
        {
            spectrumRows.push_back (analysis->spectrum[(size_t) i].data());
            aperiodicityRows.push_back (analysis->aperiodicity[(size_t) i].data());
        }

        CheapTrick (x.data(), (int) x.size(), fs, times.data(), analysis->f0.data(), frames,
                    &cheapTrick, spectrumRows.data());

        D4COption d4c;
        InitializeD4COption (&d4c);
        D4C (x.data(), (int) x.size(), fs, times.data(), analysis->f0.data(), frames,
             analysis->fftSize, &d4c, aperiodicityRows.data());

        return analysis;
    }

    // The analyses of the stretches sung most recently. A phrase repeats its
    // syllables, and a pattern is re-sung whole whenever one note of it
    // changes, so most of a re-render is syllables analysed a moment ago. An
    // analysis is a couple of MB, so the list is short.
    std::shared_ptr<const Analysis> analyseCached (const juce::File& file, double startMs, double endMs,
                                                  const std::vector<double>& x, int fs)
    {
        static std::mutex mutex;
        static std::list<std::pair<juce::String, std::shared_ptr<const Analysis>>> recent;
        constexpr size_t capacity = 48;

        const auto key = file.getFullPathName() + "|" + juce::String (file.getLastModificationTime().toMilliseconds())
                         + "|" + juce::String (startMs, 3) + "|" + juce::String (endMs, 3);

        {
            const std::lock_guard<std::mutex> lock (mutex);

            for (auto it = recent.begin(); it != recent.end(); ++it)
                if (it->first == key)
                {
                    recent.splice (recent.begin(), recent, it);
                    return it->second;
                }
        }

        auto analysis = analyse (x, fs);

        const std::lock_guard<std::mutex> lock (mutex);
        recent.emplace_front (key, analysis);

        while (recent.size() > capacity)
            recent.pop_back();

        return analysis;
    }

    template <typename Row>
    void interpolateRow (const std::vector<Row>& rows, double frame, std::vector<double>& out)
    {
        const auto last = (int) rows.size() - 1;
        const auto i0 = juce::jlimit (0, last, (int) std::floor (frame));
        const auto i1 = juce::jmin (last, i0 + 1);
        const auto a = juce::jlimit (0.0, 1.0, frame - (double) i0);

        const auto& r0 = rows[(size_t) i0];
        const auto& r1 = rows[(size_t) i1];
        out.resize (r0.size());

        for (size_t b = 0; b < r0.size(); ++b)
            out[b] = r0[b] * (1.0 - a) + r1[b] * a;
    }
}

std::optional<RenderedNote> renderNote (const NoteRender& request, juce::String& error)
{
    const auto& entry = request.entry;
    const auto fileLengthMs = fileLengthMsOf (entry.file);

    if (fileLengthMs <= 0.0)
    {
        error = "Cannot read " + entry.file.getFileName();
        return std::nullopt;
    }

    // A shortened preutterance enters the recording later by the same amount,
    // and takes that much off the fixed (consonant) part, so the note still
    // lands on the same point of the recording.
    const auto shift = juce::jmax (0.0, entry.preutterMs - request.preutterMs);
    const auto startMs = entry.offsetMs + shift;
    const auto endMs = juce::jmax (startMs + 2.0 * framePeriodMs, cutoffEndMs (entry, fileLengthMs));
    const auto consonantMs = juce::jmax (0.0, entry.consonantMs - shift);

    int fs = 44100;
    double unusedLength = 0.0;
    const auto x = readRegion (entry.file, startMs, endMs, fs, unusedLength, error);

    if (x.size() < (size_t) (fs * 3 * framePeriodMs / 1000.0))
    {
        if (error.isEmpty())
            error = "Region too short in " + entry.file.getFileName();

        return std::nullopt;
    }

    const auto analysis = analyseCached (entry.file, startMs, endMs, x, fs);
    const auto sourceMs = 1000.0 * (double) x.size() / fs;
    const auto sourceFrames = analysis->getNumFrames();

    // Output time to source time: the fixed part plays as recorded, and the
    // rest is stretched to fill the note when the note is longer than the
    // recording -- never squeezed, so a short note sings the start of the
    // vowel at its natural speed rather than a sped-up whole one.
    const auto vowelSourceMs = juce::jmax (framePeriodMs, sourceMs - consonantMs);

    auto sourceTimeFor = [&] (double outMs)
    {
        if (outMs <= consonantMs)
            return outMs;

        const auto vowelOutMs = juce::jmax (framePeriodMs, request.lengthMs - consonantMs);
        const auto into = outMs - consonantMs;

        if (vowelOutMs <= vowelSourceMs)
            return consonantMs + into;

        return consonantMs + into * vowelSourceMs / vowelOutMs;
    };

    // The pitch the note is sung at: its own, and when it follows straight on
    // from another, a glide from that one across the note's start -- 40 ms
    // either side, eased, in log frequency.
    const auto noteStartMs = request.preutterMs;
    constexpr double glideMs = 40.0;

    auto targetHzAt = [&] (double outMs)
    {
        if (! request.fromHz || *request.fromHz <= 0.0)
            return request.targetHz;

        const auto a = juce::jlimit (0.0, 1.0, (outMs - (noteStartMs - glideMs)) / (2.0 * glideMs));
        const auto eased = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * a);
        return std::exp (std::log (*request.fromHz) * (1.0 - eased) + std::log (request.targetHz) * eased);
    };

    const auto outFrames = juce::jmax (2, (int) std::ceil (request.lengthMs / framePeriodMs) + 1);
    const auto bins = (size_t) analysis->fftSize / 2 + 1;

    // The voice's colouring, each a no-op at its default.
    //
    // Gender warps the frequency axis of the envelope: reading it from a
    // higher frequency than the one being written moves every formant down,
    // which is what a longer vocal tract does. +-100 is about a fifth.
    const auto formantRatio = std::pow (2.0, request.gender / 100.0 * 0.6);

    // Breath scales the aperiodicity -- the share of each band that is noise
    // rather than voice -- from a quarter to four times as recorded.
    const auto breathFactor = std::pow (2.0, (request.breath - 50.0) / 25.0);

    // Brightness tilts the envelope about 1 kHz, up to 6 dB an octave.
    const auto tiltDbPerOctave = request.brightness / 100.0 * 6.0;

    // Modulation keeps that share of the recording's own pitch movement
    // around its median, on top of the note's pitch.
    const auto modulation = juce::jlimit (0.0, 1.0, request.modulation / 100.0);
    double sourceMedianHz = 0.0;

    if (modulation > 0.0)
    {
        std::vector<double> voiced;

        for (auto hz : analysis->f0)
            if (hz > 0.0)
                voiced.push_back (hz);

        if (! voiced.empty())
        {
            std::nth_element (voiced.begin(), voiced.begin() + (long) voiced.size() / 2, voiced.end());
            sourceMedianHz = voiced[voiced.size() / 2];
        }
    }

    const bool warps = std::abs (formantRatio - 1.0) > 1.0e-9;
    const bool tilts = std::abs (tiltDbPerOctave) > 1.0e-9;
    const bool breathes = std::abs (breathFactor - 1.0) > 1.0e-9;
    std::vector<double> tilt;

    if (tilts)
    {
        tilt.resize (bins, 1.0);

        for (size_t b = 1; b < bins; ++b)
        {
            const auto hz = juce::jmax (100.0, (double) b * fs / analysis->fftSize);
            tilt[b] = std::pow (10.0, tiltDbPerOctave * std::log2 (hz / 1000.0) / 10.0);   // a power spectrum
        }

        tilt[0] = tilt[1];
    }

    // The envelope read at `ratio` times each bin's frequency, linearly.
    auto warp = [bins, formantRatio] (std::vector<double>& row, bool isAperiodicity)
    {
        const auto source = row;

        for (size_t b = 0; b < bins; ++b)
        {
            const auto at = (double) b * formantRatio;
            const auto i0 = (size_t) at;

            if (i0 + 1 >= bins)
                row[b] = isAperiodicity ? source[bins - 1] : source[bins - 1] * 1.0e-3;   // above the top: almost nothing
            else
                row[b] = source[i0] + (source[i0 + 1] - source[i0]) * (at - (double) i0);
        }
    };

    std::vector<double> f0 ((size_t) outFrames, 0.0);
    std::vector<std::vector<double>> spectrum ((size_t) outFrames), aperiodicity ((size_t) outFrames);

    for (int k = 0; k < outFrames; ++k)
    {
        const auto outMs = k * framePeriodMs;
        const auto sourceFrame = juce::jlimit (0.0, (double) (sourceFrames - 1), sourceTimeFor (outMs) / framePeriodMs);

        auto& sp = spectrum[(size_t) k];
        auto& ap = aperiodicity[(size_t) k];
        interpolateRow (analysis->spectrum, sourceFrame, sp);
        interpolateRow (analysis->aperiodicity, sourceFrame, ap);

        if (warps)
        {
            warp (sp, false);
            warp (ap, true);
        }

        if (tilts)
            for (size_t b = 0; b < bins; ++b)
                sp[b] *= tilt[b];

        if (breathes)
            for (size_t b = 0; b < bins; ++b)
                ap[b] = juce::jlimit (0.001, 0.999999, ap[b] * breathFactor);

        // Voicing follows the recording: a consonant stays unvoiced, and
        // only what was sung with pitch is given the note's.
        const auto nearest = juce::jlimit (0, sourceFrames - 1, (int) std::lround (sourceFrame));
        const auto sourceHz = analysis->f0[(size_t) nearest];
        auto hz = sourceHz > 0.0 ? targetHzAt (outMs) : 0.0;

        if (hz > 0.0 && sourceMedianHz > 0.0)
            hz *= std::pow (sourceHz / sourceMedianHz, modulation);

        f0[(size_t) k] = hz;
    }

    std::vector<const double*> spectrumRows, aperiodicityRows;

    for (int k = 0; k < outFrames; ++k)
    {
        jassert (spectrum[(size_t) k].size() == bins);
        spectrumRows.push_back (spectrum[(size_t) k].data());
        aperiodicityRows.push_back (aperiodicity[(size_t) k].data());
    }

    const auto outSamples = juce::jmax (1, (int) std::floor ((outFrames - 1) * framePeriodMs * 0.001 * fs) + 1);
    std::vector<double> y ((size_t) outSamples, 0.0);

    Synthesis (f0.data(), outFrames, spectrumRows.data(), aperiodicityRows.data(),
               analysis->fftSize, framePeriodMs, fs, outSamples, y.data());

    RenderedNote rendered;
    rendered.sampleRate = fs;

    const auto wanted = juce::jmax (1, (int) std::llround (request.lengthMs * 0.001 * fs));
    rendered.audio.setSize (1, wanted);
    rendered.audio.clear();

    auto* out = rendered.audio.getWritePointer (0);
    const auto copied = juce::jmin (wanted, outSamples);

    for (int i = 0; i < copied; ++i)
        out[i] = (float) y[(size_t) i] * request.gain;

    // The crossfades with the notes either side: in over this one's overlap,
    // out under the next one's. Equal-gain: the two sides are the same voice
    // and largely in phase, so equal power would bulge in the middle.
    const auto fadeIn = juce::jmin (wanted, (int) std::llround (request.fadeInMs * 0.001 * fs));
    const auto fadeOut = juce::jmin (wanted, (int) std::llround (request.fadeOutMs * 0.001 * fs));

    for (int i = 0; i < fadeIn; ++i)
        out[i] *= (float) i / (float) fadeIn;

    for (int i = 0; i < fadeOut; ++i)
        out[wanted - 1 - i] *= (float) i / (float) fadeOut;

    return rendered;
}

std::optional<RawSample> readRawSample (const OtoEntry& entry, juce::String& error)
{
    const auto fileLengthMs = fileLengthMsOf (entry.file);

    if (fileLengthMs <= 0.0)
    {
        error = "Cannot read " + entry.file.getFileName();
        return std::nullopt;
    }

    int fs = 44100;
    double unusedLength = 0.0;
    const auto x = readRegion (entry.file, entry.offsetMs, cutoffEndMs (entry, fileLengthMs), fs, unusedLength, error);

    if (x.empty())
        return std::nullopt;

    RawSample sample;
    sample.sampleRate = fs;
    sample.startMs = entry.consonantMs;
    sample.audio.setSize (1, (int) x.size());

    for (size_t i = 0; i < x.size(); ++i)
        sample.audio.setSample (0, (int) i, (float) x[i]);

    std::vector<double> times;
    auto f0 = estimateF0 (x, fs, times);
    f0.erase (std::remove_if (f0.begin(), f0.end(), [] (double hz) { return hz <= 0.0; }), f0.end());

    if (! f0.empty())
    {
        std::nth_element (f0.begin(), f0.begin() + (long) f0.size() / 2, f0.end());
        sample.hz = f0[f0.size() / 2];
    }

    return sample;
}

} // namespace carve::voice
