#pragma once

#include <memory>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace orionish
{

// tracktion derives the settings folder from the name the Engine is built with,
// so this is what decides where the scanned plugin list, device setup and the
// rest are kept. The GUI and the renderer are one application and must agree:
// when they passed different names they silently kept separate stores, and a
// plugin list built by "orionish-te-render --scan" was invisible to the GUI.
inline constexpr const char* applicationName = "Orionish TE";

// The app never records, so it has no reason to open an audio input — and
// opening one is actively dangerous with JUCE 8.0.6 on macOS.
//
// When the default input and output are different devices, JUCE builds an
// AudioIODeviceCombiner and asks both for the same block size. If a device
// refuses that size, CoreAudioInternal::reopen() sizes its de-interleave temp
// buffers from the device's real block size (via updateDetailsFromDevice ->
// allocateTempBuffers) but then "bodges" bufferSize back to the *requested*
// size without reallocating. The next audio callback de-interleaves the
// requested number of samples into the smaller buffer and corrupts the heap;
// the process then dies in whatever allocates next.
//
// A Bluetooth headset triggers this reliably: its input runs at 16kHz / 320
// frames while 512 is requested, so 512 floats go into a 324-float buffer.
struct PlaybackOnlyEngineBehaviour : te::EngineBehaviour
{
    bool shouldOpenAudioInputByDefault() override  { return false; }

    // Scan plugins in a child process. A scan loads arbitrary third-party code
    // and plenty of plugins crash on load or on destruction; in-process that
    // takes the whole app down mid-scan. The engine restarts the child, gives
    // the plugin a second chance with a fresh one (the real culprit is often
    // the plugin before it) and blacklists it if it crashes again.
    //
    // Requires PluginManager::startChildProcessPluginScan at the top of
    // JUCEApplication::initialise, which is where the child process learns that
    // it is one. The console renderer has no message loop to run a scan child,
    // so it leaves this off and scans in-process.
    bool canScanPluginsOutOfProcess() override  { return scanOutOfProcess; }

    explicit PlaybackOnlyEngineBehaviour (bool scanOutOfProcessToUse = false)
        : scanOutOfProcess (scanOutOfProcessToUse) {}

    const bool scanOutOfProcess;
};

// The engine hands long jobs (rendering) to the UI to run behind a progress
// bar. With no UI to put one in, run them inline on the calling thread — the
// default implementation is a jassertfalse that silently does nothing, which
// would make Renderer::renderToFile return an empty file.
struct HeadlessUIBehaviour : te::UIBehaviour
{
    void runTaskWithProgressBar (te::ThreadPoolJobWithProgress& job) override
    {
        while (job.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain)
        {}
    }
};

inline std::unique_ptr<te::Engine> createEngine (std::unique_ptr<te::UIBehaviour> uiBehaviour = nullptr,
                                                 bool scanPluginsOutOfProcess = false)
{
    return std::make_unique<te::Engine> (
        applicationName, std::move (uiBehaviour),
        std::make_unique<PlaybackOnlyEngineBehaviour> (scanPluginsOutOfProcess));
}

} // namespace orionish
