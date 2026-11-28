#pragma once

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include <juce_core/juce_core.h>

// Timing for finding out where the message thread goes, on a device as much
// as on the desktop. Off unless the process is started with CARVE_PERF set
// (on the iPad: devicectl's --environment-variables), when every probe that
// ran prints a line a second to stderr:
//
//   perf roll.drag      n=  58  avg  2.10 ms  max  6.40 ms  total 121.8 ms/s
//
// Message thread only: the tallies are plain, not atomic.
namespace carve::perf
{

inline bool enabled()
{
    static const bool on = std::getenv ("CARVE_PERF") != nullptr;
    return on;
}

struct Tally
{
    int count = 0;
    double totalMs = 0.0, maxMs = 0.0;
};

inline void record (const char* name, double ms)
{
    static std::map<std::string, Tally> tallies;
    static double windowStart = juce::Time::getMillisecondCounterHiRes();

    auto& t = tallies[name];
    ++t.count;
    t.totalMs += ms;
    t.maxMs = std::max (t.maxMs, ms);

    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto elapsed = now - windowStart;

    if (elapsed < 1000.0)
        return;

    for (auto& [key, tally] : tallies)
        if (tally.count > 0)
            std::fprintf (stderr, "perf %-20s n=%4d  avg %6.2f ms  max %6.2f ms  total %6.1f ms/s\n",
                          key.c_str(), tally.count, tally.totalMs / tally.count, tally.maxMs,
                          tally.totalMs * 1000.0 / elapsed);

    std::fprintf (stderr, "perf ----\n");
    std::fflush (stderr);
    tallies.clear();
    windowStart = now;
}

// Times its own lifetime under `name`.
class Scope
{
public:
    explicit Scope (const char* nameToUse)
        : name (nameToUse), start (enabled() ? juce::Time::getMillisecondCounterHiRes() : 0.0) {}

    ~Scope()
    {
        if (enabled())
            record (name, juce::Time::getMillisecondCounterHiRes() - start);
    }

private:
    const char* name;
    double start;

    JUCE_DECLARE_NON_COPYABLE (Scope)
};

// The time since this probe last ran: how often an event arrives, and the
// gaps where the thread was busy elsewhere.
inline void interval (const char* name)
{
    if (! enabled())
        return;

    static std::map<std::string, double> last;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    auto& previous = last[name];

    if (previous > 0.0 && now - previous < 1000.0)
        record (name, now - previous);

    previous = now;
}

} // namespace carve::perf
