#include "ReferencePlayerPlugin.h"

#include <algorithm>

#include <juce_dsp/juce_dsp.h>

namespace carve::plugins
{

namespace
{
    constexpr double crossfadeSeconds = 0.015;      // A to B and back, without a click
    constexpr double longestSeconds = 20.0 * 60.0;  // a file longer than this is read only this far
    constexpr double maxMatchDb = 30.0;

    // The average spectrum of some audio: Hann-windowed frames, half
    // overlapped, their power averaged, mid (L+R)/2 as the analyzer sees a
    // mix, scaled as the analyzer scales its own (a full-scale sine is 0 dB).
    std::shared_ptr<const ReferencePlayerPlugin::Spectrum> averageSpectrum (const juce::AudioBuffer<float>& audio, int numSamples, double rate)
    {
        auto spectrum = std::make_shared<ReferencePlayerPlugin::Spectrum>();
        spectrum->sampleRate = rate;
        const int size = 1 << spectrum->fftOrder, bins = size / 2 + 1;
        juce::dsp::FFT fft (spectrum->fftOrder);
        juce::dsp::WindowingFunction<float> window ((size_t) size, juce::dsp::WindowingFunction<float>::hann, false);
        std::vector<float> frame ((size_t) size * 2);
        std::vector<double> power ((size_t) bins, 0.0);
        int frames = 0;

        const auto* left = audio.getReadPointer (0);
        const auto* right = audio.getReadPointer (1);

        for (int start = 0; start + size <= numSamples; start += size / 2)
        {
            for (int i = 0; i < size; ++i)
                frame[(size_t) i] = 0.5f * (left[start + i] + right[start + i]);

            std::fill (frame.begin() + size, frame.end(), 0.0f);
            window.multiplyWithWindowingTable (frame.data(), (size_t) size);
            fft.performFrequencyOnlyForwardTransform (frame.data());

            for (int b = 0; b < bins; ++b)
                power[(size_t) b] += (double) frame[(size_t) b] * frame[(size_t) b];

            ++frames;
        }

        spectrum->db.resize ((size_t) bins);
        const double norm = 2.0 / (size * 0.5);

        for (int b = 0; b < bins; ++b)
            spectrum->db[(size_t) b] = (float) (10.0 * std::log10 (power[(size_t) b] / juce::jmax (1, frames) * norm * norm + 1.0e-20));

        return spectrum;
    }
} // namespace

const char* ReferencePlayerPlugin::xmlTypeName = "carveReferencePlayer";

ReferencePlayerPlugin::ReferencePlayerPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    loader.startThread (juce::Thread::Priority::low);
}

ReferencePlayerPlugin::~ReferencePlayerPlugin()
{
    notifyListenersOfDeletion();
    loader.stopThread (5000);
}

//==============================================================================
void ReferencePlayerPlugin::setClips (std::vector<Clip> clips, float trim, bool levelMatched, double shift)
{
    trimDb.store (trim, std::memory_order_relaxed);
    matched.store (levelMatched, std::memory_order_relaxed);
    timeShift.store (shift, std::memory_order_relaxed);

    std::sort (clips.begin(), clips.end(), [] (const Clip& a, const Clip& b) { return a.start < b.start; });

    {
        const juce::ScopedLock sl (requestLock);

        if (clips == wantedClips)
            return;

        wantedClips = std::move (clips);
    }

    loader.notify();
}

void ReferencePlayerPlugin::setListening (bool shouldPlayReference)
{
    listening.store (shouldPlayReference, std::memory_order_relaxed);
}

std::shared_ptr<const ReferencePlayerPlugin::Program> ReferencePlayerPlugin::getProgram() const
{
    const juce::SpinLock::ScopedLockType sl (publishLock);
    return published;
}

std::shared_ptr<const ReferencePlayerPlugin::Spectrum> ReferencePlayerPlugin::getReferenceSpectrum() const
{
    const auto program = getProgram();
    return program != nullptr ? program->spectrum : nullptr;
}

//==============================================================================
void ReferencePlayerPlugin::Loader::run()
{
    while (! threadShouldExit())
    {
        owner.buildIfStale();
        wait (-1);
    }
}

std::shared_ptr<const ReferencePlayerPlugin::Audio> ReferencePlayerPlugin::readFile (const juce::File& file, double rate)
{
    const auto key = file.getFullPathName() + "@" + juce::String (rate);

    if (const auto it = files.find (key); it != files.end())
        return it->second;

    std::unique_ptr<juce::AudioFormatReader> reader (
        engine.getAudioFileFormatManager().readFormatManager.createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return nullptr;

    const auto length = (int) juce::jmin ((juce::int64) (longestSeconds * reader->sampleRate), reader->lengthInSamples);
    juce::AudioBuffer<float> source (2, length);
    reader->read (&source, 0, length, 0, true, reader->numChannels > 1);

    if (reader->numChannels == 1)
        source.copyFrom (1, 0, source, 0, 0, length);

    auto audio = std::make_shared<Audio>();
    audio->file = file;
    audio->sampleRate = rate;

    // At the device's rate, once, so playing it is a copy.
    if (std::abs (reader->sampleRate - rate) < 1.0e-6)
        audio->samples = std::move (source);
    else
    {
        const auto ratio = reader->sampleRate / rate;
        const auto outLength = juce::jmax (1, (int) std::floor ((length - 4) / ratio));
        audio->samples.setSize (2, outLength);

        for (int ch = 0; ch < 2; ++ch)
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.process (ratio, source.getReadPointer (ch), audio->samples.getWritePointer (ch), outLength);
        }
    }

    files[key] = audio;
    return audio;
}

void ReferencePlayerPlugin::buildIfStale()
{
    std::vector<Clip> clips;
    double rate;

    {
        const juce::ScopedLock sl (requestLock);
        clips = wantedClips;
        rate = wantedRate;
    }

    if (const auto current = getProgram(); current != nullptr ? current->clips == clips && std::abs (current->sampleRate - rate) < 1.0e-6
                                                                : clips.empty())
        return;

    auto publish = [this] (std::shared_ptr<const Program> program)
    {
        {
            const juce::SpinLock::ScopedLockType sl (publishLock);
            published = program;
        }

        retained.erase (std::remove_if (retained.begin(), retained.end(),
                                        [] (const auto& held) { return held.use_count() == 1; }),
                        retained.end());

        if (program != nullptr)
            retained.push_back (std::move (program));
    };

    if (clips.empty() || rate <= 0.0)
    {
        publish (nullptr);
        referenceLufs.store (-INFINITY, std::memory_order_relaxed);
        status.store (clips.empty() ? Status::none : Status::loading, std::memory_order_relaxed);
        return;
    }

    status.store (Status::loading, std::memory_order_relaxed);

    auto program = std::make_shared<Program>();
    program->clips = clips;
    program->sampleRate = rate;
    bool anyFailed = false;

    for (const auto& clip : clips)
    {
        if (loader.threadShouldExit())
            return;

        auto audio = readFile (clip.file, rate);

        if (audio == nullptr)
        {
            anyFailed = true;
            continue;
        }

        Program::Placed placed;
        placed.start = (juce::int64) std::llround (clip.start * rate);
        placed.length = (juce::int64) std::llround (clip.length * rate);
        placed.fileOffset = (juce::int64) std::llround (clip.fileOffset * rate);
        placed.audio = std::move (audio);
        program->placed.push_back (std::move (placed));
    }

    // Files no longer placed anywhere are let go of.
    for (auto it = files.begin(); it != files.end();)
    {
        const bool used = std::any_of (program->placed.begin(), program->placed.end(),
                                       [&it] (const Program::Placed& p) { return p.audio == it->second; });
        it = used ? std::next (it) : files.erase (it);
    }

    // Loudness and spectrum of what is placed, as it is placed: the clips one
    // after another, up to the length a single file may be.
    {
        auto playable = [] (const Program::Placed& p)
        {
            return juce::jmax ((juce::int64) 0, juce::jmin (p.length, (juce::int64) p.audio->samples.getNumSamples() - p.fileOffset));
        };

        juce::int64 total = 0;

        for (const auto& p : program->placed)
            total += playable (p);

        total = juce::jmin (total, (juce::int64) (longestSeconds * rate));
        juce::AudioBuffer<float> joined (2, (int) juce::jmax ((juce::int64) 1, total));
        joined.clear();
        juce::int64 at = 0;

        for (const auto& p : program->placed)
        {
            const auto n = juce::jmin (playable (p), total - at);

            for (int ch = 0; ch < 2 && n > 0; ++ch)
                joined.copyFrom (ch, (int) at, p.audio->samples, ch, (int) p.fileOffset, (int) n);

            at += n;
        }

        const float* channels[] { joined.getReadPointer (0), joined.getReadPointer (1) };
        program->lufs = at > 0 ? LoudnessMeter::measure (channels, 2, (int) at, rate) : -INFINITY;
        program->spectrum = at > 0 ? averageSpectrum (joined, (int) at, rate) : nullptr;
    }

    // Asked for something else meanwhile: that, rather than this.
    {
        const juce::ScopedLock sl (requestLock);

        if (! (wantedClips == clips) || std::abs (wantedRate - rate) > 1.0e-6)
        {
            loader.notify();
            return;
        }
    }

    referenceLufs.store (program->lufs, std::memory_order_relaxed);
    publish (std::move (program));
    status.store (anyFailed ? Status::failed : Status::ready, std::memory_order_relaxed);
}

//==============================================================================
void ReferencePlayerPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    mixMeter.prepare (info.sampleRate);
    wasPlaying = false;

    {
        const juce::ScopedLock sl (requestLock);

        if (std::abs (wantedRate - info.sampleRate) < 1.0e-6)
            return;

        wantedRate = info.sampleRate;
    }

    loader.notify();
}

void ReferencePlayerPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    // An export is the mix.
    if (fc.isRendering)
        return;

    {
        const juce::SpinLock::ScopedTryLockType sl (publishLock);

        if (sl.isLocked())
            live = published;
    }

    auto& buffer = *fc.destBuffer;
    const int numChannels = juce::jmin (2, buffer.getNumChannels());
    const int n = fc.bufferNumSamples;

    if (numChannels == 0 || n <= 0)
        return;

    float* channels[2] {};
    for (int ch = 0; ch < numChannels; ++ch)
        channels[ch] = buffer.getWritePointer (ch, fc.bufferStartSample);

    // Each run of playback measures the mix afresh -- it may have changed --
    // keeping the last run's figure until this one has one of its own.
    const bool playing = fc.isPlaying;

    if (playing && ! wasPlaying)
    {
        if (mixMeter.hasMeasured())
            lastMixLufs = mixMeter.integrated();

        mixMeter.reset();
    }

    wasPlaying = playing;

    const bool reference = playing && live != nullptr && listening.load (std::memory_order_relaxed);

    // The mix is measured while it is what is heard, and only then.
    if (playing && ! reference && mixAmount >= 1.0f)
        mixMeter.process (channels, numChannels, n);

    const auto mix = mixMeter.hasMeasured() ? mixMeter.integrated() : lastMixLufs;
    mixLufs.store (mix, std::memory_order_relaxed);

    double gainDb = trimDb.load (std::memory_order_relaxed);

    if (live != nullptr && matched.load (std::memory_order_relaxed) && std::isfinite (mix) && std::isfinite (live->lufs))
        gainDb += juce::jlimit (-maxMatchDb, maxMatchDb, mix - live->lufs);

    appliedGainDb.store (gainDb, std::memory_order_relaxed);

    const float target = reference ? 0.0f : 1.0f;

    if (mixAmount >= 1.0f && target >= 1.0f)
        return;   // A, and not on the way back from B

    const auto first = (juce::int64) std::llround ((fc.editTime.getStart().inSeconds()
                                                    + timeShift.load (std::memory_order_relaxed)) * sampleRate);

    const auto fadeStep = (float) (1.0 / (crossfadeSeconds * sampleRate));
    const auto gainTarget = juce::Decibels::decibelsToGain (gainDb);
    const auto gainStep = (gainTarget - gainNow) / n;
    const Program::Placed* clip = nullptr;

    for (int i = 0; i < n; ++i)
    {
        mixAmount = target < mixAmount ? juce::jmax (target, mixAmount - fadeStep) : juce::jmin (target, mixAmount + fadeStep);
        gainNow += gainStep;

        // The clip under the playhead; a gap between clips is silence.
        const auto at = first + i;

        if (live != nullptr && (clip == nullptr || at < clip->start || at >= clip->start + clip->length))
        {
            clip = nullptr;

            for (const auto& p : live->placed)
                if (at >= p.start && at < p.start + p.length)
                {
                    clip = &p;
                    break;
                }
        }

        const auto inFile = clip != nullptr ? clip->fileOffset + (at - clip->start) : (juce::int64) -1;
        const bool inside = clip != nullptr && inFile >= 0 && inFile < clip->audio->samples.getNumSamples();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto ref = inside ? clip->audio->samples.getSample (ch, (int) inFile) * (float) gainNow : 0.0f;
            channels[ch][i] = channels[ch][i] * mixAmount + ref * (1.0f - mixAmount);
        }
    }

    gainNow = gainTarget;
}

} // namespace carve::plugins
