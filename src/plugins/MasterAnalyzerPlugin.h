#pragma once

#include <array>
#include <atomic>

#include <tracktion_engine/tracktion_engine.h>

#include "Loudness.h"

namespace te = tracktion;

namespace carve::plugins
{

// What the mastering tools measure: the last plugin on the master, after the
// reference player, so it hears exactly what comes out -- the mix in A, the
// reference in B. Never in a render.
//
// The master fader is applied after every plugin in the master's list, so its
// gain is handed in (setMonitorGainDb) and applied to what is measured: the
// readings are post-fader, as the ears get them.
//
// The audio thread measures loudness (momentary, short-term, integrated, LRA),
// true peak, sample peak, correlation and balance, and keeps the last few
// thousand samples and the last minute of loudness in rings that the
// Analyzer window draws from. Nothing is allocated or locked.
class MasterAnalyzerPlugin : public te::Plugin
{
public:
    explicit MasterAnalyzerPlugin (te::PluginCreationInfo);
    ~MasterAnalyzerPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Master Analyzer"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Master Analyzer"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Analyzer"; }
    juce::String getSelectableDescription() override  { return TRANS ("Master Analyzer"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override {}
    void applyToBuffer (const te::PluginRenderContext&) override;

    void setMonitorGainDb (float db)  { monitorGain.store (juce::Decibels::decibelsToGain (db), std::memory_order_relaxed); }

    // Starts the integrated loudness, the range and the peaks over. Playback
    // starting does too, if resetOnPlay.
    void requestReset()                 { resetRequested.store (true, std::memory_order_relaxed); }
    void setResetOnPlay (bool shouldReset)  { resetOnPlay.store (shouldReset, std::memory_order_relaxed); }

    struct Readings
    {
        double momentary = -INFINITY, shortTerm = -INFINITY, integrated = -INFINITY, range = 0.0;
        double truePeakDb = -INFINITY;          // since the reset
        double samplePeakDb[2] { -INFINITY, -INFINITY };   // the last block's, per channel
        double correlation = 1.0;               // -1 .. +1, over the last 300ms
        double balance = 0.0;                   // -1 left .. +1 right
        double sampleRate = 48000.0;
    };

    Readings getReadings() const;

    // The last samples heard, newest last, as left and right; `count` up to
    // ringSize. Read while being written: a torn read is a frame drawn a
    // block late.
    static constexpr int ringSize = 16384;
    void copyRecentSamples (float* left, float* right, int count) const;

    // The last minute of loudness, one entry every 100ms, newest last.
    static constexpr int historySize = 600;
    void copyHistory (float* momentary, float* shortTerm, int count) const;

private:
    std::atomic<float> monitorGain { 1.0f };
    std::atomic<bool> resetRequested { false }, resetOnPlay { true };

    // Published readings.
    std::atomic<double> momentaryNow { -INFINITY }, shortTermNow { -INFINITY }, integratedNow { -INFINITY },
                        rangeNow { 0.0 }, truePeakNow { -INFINITY }, correlationNow { 1.0 }, balanceNow { 0.0 };
    std::array<std::atomic<double>, 2> samplePeakNow;

    std::array<std::array<std::atomic<float>, ringSize>, 2> ring;
    std::atomic<int> ringWrite { 0 };

    std::array<std::array<std::atomic<float>, historySize>, 2> history;
    std::atomic<int> historyWrite { 0 };

    // Audio thread only.
    LoudnessMeter loudness;
    TruePeakMeter truePeak;
    long long lastStep = 0;
    bool wasPlaying = false;
    double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0, decay = 0.999;
    std::array<float, 4096> scratchL {}, scratchR {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterAnalyzerPlugin)
};

} // namespace carve::plugins
