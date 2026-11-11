#include "MasterAnalyzerPlugin.h"

namespace carve::plugins
{

const char* MasterAnalyzerPlugin::xmlTypeName = "carveMasterAnalyzer";

MasterAnalyzerPlugin::MasterAnalyzerPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    for (auto& p : samplePeakNow)
        p.store (-INFINITY, std::memory_order_relaxed);

    for (auto& channel : ring)
        for (auto& v : channel)
            v.store (0.0f, std::memory_order_relaxed);

    for (auto& channel : history)
        for (auto& v : channel)
            v.store (-INFINITY, std::memory_order_relaxed);
}

MasterAnalyzerPlugin::~MasterAnalyzerPlugin()
{
    notifyListenersOfDeletion();
}

void MasterAnalyzerPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    loudness.prepare (info.sampleRate);
    truePeak.reset();
    lastStep = 0;
    wasPlaying = false;

    // Correlation over roughly the last 300ms.
    decay = std::exp (-1.0 / (0.3 * info.sampleRate));
    sumLR = sumLL = sumRR = 0.0;
}

MasterAnalyzerPlugin::Readings MasterAnalyzerPlugin::getReadings() const
{
    Readings r;
    r.momentary = momentaryNow.load (std::memory_order_relaxed);
    r.shortTerm = shortTermNow.load (std::memory_order_relaxed);
    r.integrated = integratedNow.load (std::memory_order_relaxed);
    r.range = rangeNow.load (std::memory_order_relaxed);
    r.truePeakDb = truePeakNow.load (std::memory_order_relaxed);
    r.samplePeakDb[0] = samplePeakNow[0].load (std::memory_order_relaxed);
    r.samplePeakDb[1] = samplePeakNow[1].load (std::memory_order_relaxed);
    r.correlation = correlationNow.load (std::memory_order_relaxed);
    r.balance = balanceNow.load (std::memory_order_relaxed);
    r.sampleRate = sampleRate;
    return r;
}

void MasterAnalyzerPlugin::copyRecentSamples (float* left, float* right, int count) const
{
    count = juce::jmin (count, ringSize);
    const int end = ringWrite.load (std::memory_order_acquire);

    for (int i = 0; i < count; ++i)
    {
        const int at = (end - count + i + ringSize) % ringSize;
        left[i] = ring[0][(size_t) at].load (std::memory_order_relaxed);
        right[i] = ring[1][(size_t) at].load (std::memory_order_relaxed);
    }
}

void MasterAnalyzerPlugin::copyHistory (float* momentary, float* shortTerm, int count) const
{
    count = juce::jmin (count, historySize);
    const int end = historyWrite.load (std::memory_order_acquire);

    for (int i = 0; i < count; ++i)
    {
        const int at = (end - count + i + historySize) % historySize;
        momentary[i] = history[0][(size_t) at].load (std::memory_order_relaxed);
        shortTerm[i] = history[1][(size_t) at].load (std::memory_order_relaxed);
    }
}

void MasterAnalyzerPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || fc.isRendering)
        return;

    SCOPED_REALTIME_CHECK

    const auto& buffer = *fc.destBuffer;
    const int numChannels = juce::jmin (2, buffer.getNumChannels());

    if (numChannels == 0)
        return;

    const bool playing = fc.isPlaying;

    if (resetRequested.exchange (false, std::memory_order_relaxed)
         || (playing && ! wasPlaying && resetOnPlay.load (std::memory_order_relaxed)))
    {
        loudness.reset();
        truePeak.reset();
        lastStep = 0;
        integratedNow.store (-INFINITY, std::memory_order_relaxed);
        rangeNow.store (0.0, std::memory_order_relaxed);
        truePeakNow.store (-INFINITY, std::memory_order_relaxed);
    }

    wasPlaying = playing;

    const auto gain = monitorGain.load (std::memory_order_relaxed);

    for (int done = 0; done < fc.bufferNumSamples;)
    {
        const int n = juce::jmin ((int) scratchL.size(), fc.bufferNumSamples - done);
        const auto* inL = buffer.getReadPointer (0, fc.bufferStartSample + done);
        const auto* inR = buffer.getReadPointer (numChannels > 1 ? 1 : 0, fc.bufferStartSample + done);

        float peakL = 0.0f, peakR = 0.0f;
        int write = ringWrite.load (std::memory_order_relaxed);

        for (int i = 0; i < n; ++i)
        {
            const auto l = inL[i] * gain, r = inR[i] * gain;
            scratchL[(size_t) i] = l;
            scratchR[(size_t) i] = r;
            peakL = juce::jmax (peakL, std::abs (l));
            peakR = juce::jmax (peakR, std::abs (r));

            sumLR = sumLR * decay + (double) l * r;
            sumLL = sumLL * decay + (double) l * l;
            sumRR = sumRR * decay + (double) r * r;

            ring[0][(size_t) write].store (l, std::memory_order_relaxed);
            ring[1][(size_t) write].store (r, std::memory_order_relaxed);
            write = (write + 1) % ringSize;
        }

        ringWrite.store (write, std::memory_order_release);

        samplePeakNow[0].store (juce::Decibels::gainToDecibels ((double) peakL, -200.0), std::memory_order_relaxed);
        samplePeakNow[1].store (juce::Decibels::gainToDecibels ((double) peakR, -200.0), std::memory_order_relaxed);

        const auto energy = std::sqrt (sumLL * sumRR);
        correlationNow.store (energy > 1.0e-12 ? sumLR / energy : 1.0, std::memory_order_relaxed);
        balanceNow.store (sumLL + sumRR > 1.0e-12 ? (std::sqrt (sumRR) - std::sqrt (sumLL)) / (std::sqrt (sumRR) + std::sqrt (sumLL)) : 0.0,
                          std::memory_order_relaxed);

        // Loudness and peaks only from what is played: a stopped transport's
        // silence would drag the integrated figure down.
        if (playing)
        {
            const float* channels[] { scratchL.data(), scratchR.data() };
            loudness.process (channels, 2, n);
            truePeak.process (channels, 2, n);

            if (const auto step = loudness.getStepsTaken(); step != lastStep)
            {
                lastStep = step;
                const int h = historyWrite.load (std::memory_order_relaxed);
                history[0][(size_t) h].store ((float) loudness.momentary(), std::memory_order_relaxed);
                history[1][(size_t) h].store ((float) loudness.shortTerm(), std::memory_order_relaxed);
                historyWrite.store ((h + 1) % historySize, std::memory_order_release);

                momentaryNow.store (loudness.momentary(), std::memory_order_relaxed);
                shortTermNow.store (loudness.shortTerm(), std::memory_order_relaxed);
                integratedNow.store (loudness.integrated(), std::memory_order_relaxed);
                rangeNow.store (loudness.loudnessRange(), std::memory_order_relaxed);
            }

            truePeakNow.store (truePeak.getPeakDb(), std::memory_order_relaxed);
        }

        done += n;
    }
}

} // namespace carve::plugins
