#pragma once

#include <memory>

#include <tracktion_engine/tracktion_engine.h>

#include "plugins/DelayPlugin.h"
#include "plugins/DistortionPlugin.h"
#include "plugins/DrumSynthPlugin.h"
#include "plugins/MeteredCompressorPlugin.h"
#include "plugins/NoteMonitorPlugin.h"
#include "plugins/SaturationPlugin.h"

namespace te = tracktion;

namespace carve
{

// tracktion derives the settings folder from the name the Engine is built with,
// so this is what decides where the scanned plugin list, device setup and the
// rest are kept. The GUI and the renderer are one application and must agree:
// when they passed different names they silently kept separate stores, and a
// plugin list built by "carve-render --scan" was invisible to the GUI.
inline constexpr const char* applicationName = "Carve DAW";

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

    // The offline renderer runs the node graph on (this - 1) worker threads,
    // and parallel execution sums in whatever order blocks finish -- float
    // addition is not associative, so two renders of the same song differ in
    // the low bits. The CLI renders single-threaded so its output is
    // bit-reproducible and regression-testable; the GUI keeps its threads,
    // because live playback shares this number.
    int getNumberOfCPUsToUseForAudio() override
    {
        return singleThreadedAudio ? 1 : te::EngineBehaviour::getNumberOfCPUsToUseForAudio();
    }

    bool singleThreadedAudio = false;

    // The piano roll previews notes through AudioTrack::playGuideNote, which
    // is gated on this flag -- and its default is false, which silently
    // swallowed every preview. The transport does not need to be rolling:
    // guide notes ride the live MIDI path, which runs whenever a playback
    // context exists (previewNote allocates one on demand).
    bool shouldPlayMidiGuideNotes() override  { return true; }

    // Every MIDI clip plays through a LoopingMidiNode rather than the older
    // MidiNode, which is what tracktion picks unless this is cleared
    // (createNodeForMidiClip keys on canUseProxy). Only the former keeps an
    // ActiveNoteList across graph rebuilds: it turns off a note deleted while
    // it sounded, and the track's CombiningNode turns off the notes of a clip
    // whose node is gone from the rebuilt graph. The MidiNode derives its
    // note-offs from whatever sequence it has now, so a deleted note or clip
    // rang on for ever. Every clip added to an Edit passes through here,
    // however it was made; carve-render --check-note-offs is the check.
    void newClipAdded (te::Clip& clip, bool) override
    {
        if (auto midi = dynamic_cast<te::MidiClip*> (&clip))
            midi->setUsesProxy (false);
    }

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

    explicit PlaybackOnlyEngineBehaviour (bool scanOutOfProcessToUse = false,
                                          bool singleThreadedAudioToUse = false)
        : singleThreadedAudio (singleThreadedAudioToUse),
          scanOutOfProcess (scanOutOfProcessToUse) {}

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

// The app was called Orionish TE before, and tracktion derives its settings
// folder from the name -- so a rename would silently orphan the scanned plugin
// list, the blacklist that keeps a crashing plugin out, and the device setup.
// Moved once, only when there is nothing in the new place to overwrite.
inline void migrateSettingsFromFormerName()
{
    auto library = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
    auto current = library.getChildFile (applicationName);
    auto former = library.getChildFile ("Orionish TE");

    if (! current.exists() && former.isDirectory())
        former.copyDirectoryTo (current);
}

inline std::unique_ptr<te::Engine> createEngine (std::unique_ptr<te::UIBehaviour> uiBehaviour = nullptr,
                                                 bool scanPluginsOutOfProcess = false,
                                                 bool singleThreadedAudio = false)
{
    migrateSettingsFromFormerName();

    auto engine = std::make_unique<te::Engine> (
        applicationName, std::move (uiBehaviour),
        std::make_unique<PlaybackOnlyEngineBehaviour> (scanPluginsOutOfProcess, singleThreadedAudio));

    // Our own plugins, registered like tracktion's own internal ones so they
    // are created from a song by type name and need no plugin scan.
    engine->getPluginManager().createBuiltInType<plugins::DistortionPlugin>();
    engine->getPluginManager().createBuiltInType<plugins::DelayPlugin>();
    engine->getPluginManager().createBuiltInType<plugins::SaturationPlugin>();
    engine->getPluginManager().createBuiltInType<plugins::MeteredCompressorPlugin>();
    engine->getPluginManager().createBuiltInType<plugins::DrumSynthPlugin>();
    engine->getPluginManager().createBuiltInType<plugins::NoteMonitorPlugin>();

    return engine;
}

} // namespace carve
