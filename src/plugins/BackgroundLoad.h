#pragma once

#include <atomic>

#include <juce_core/juce_core.h>

namespace carve::plugins
{

// The time a plugin spends working on threads of its own -- rendering strokes
// or notes ahead, re-rendering a sample after an edit, loading -- which
// te::Plugin::getCpuUsage() cannot see: tracktion times only applyToBuffer on
// the audio thread. Kept as a running total of wall-clock seconds; whoever
// wants a load samples it twice and divides by the time in between (the
// mixer's channel strip does), which comes out in the same units as
// getCpuUsage(): the share of one core.
class BackgroundTime
{
public:
    double getSeconds() const noexcept
    {
        return juce::Time::highResolutionTicksToSeconds (ticks.load (std::memory_order_relaxed));
    }

    // Adds its own lifetime to the total. Any thread.
    class Scope
    {
    public:
        explicit Scope (BackgroundTime& t) noexcept : time (t), start (juce::Time::getHighResolutionTicks()) {}
        ~Scope()   { time.ticks.fetch_add (juce::Time::getHighResolutionTicks() - start, std::memory_order_relaxed); }

    private:
        BackgroundTime& time;
        const juce::int64 start;
        JUCE_DECLARE_NON_COPYABLE (Scope)
    };

private:
    std::atomic<juce::int64> ticks { 0 };
};

// A plugin that does work off the audio thread says how much, so the mixer
// can add it to the strip's load (see ChannelStrip::updateLoad).
struct BackgroundWorker
{
    virtual ~BackgroundWorker() = default;

    // The seconds spent so far, in total. Any thread.
    virtual double getBackgroundSeconds() const noexcept = 0;
};

} // namespace carve::plugins
