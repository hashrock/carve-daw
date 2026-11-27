#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include <tracktion_engine/tracktion_engine.h>

#include "RenderThread.h"
#include "sync/RenderPreroll.h"

namespace te = tracktion;

namespace carve::app
{

// One generator's track rendered to a file, in the background: the bounce
// that turns a part into a loop to chop up again.
//
// The same render an export's stem is -- the track through its own effects,
// with the return busses so its sends come back with it, and none of the
// master's plugins -- run the same way (see ExportComponent for why it is a
// thread of its own, and RenderPreroll.h for why it starts early). Whatever
// the Edit is playing is what is rendered: the caller puts the right thing in
// it first, and onRenderingChanged is told when it can put it back.
//
// A track inside a group cannot be rendered alone: its audio leaves through
// the bus, and a bus pulls in everything that feeds it. start() refuses one.
class TrackBouncer : private juce::Timer
{
public:
    explicit TrackBouncer (te::Edit&);
    ~TrackBouncer() override;

    bool isBouncing() const  { return renderThread != nullptr; }

    // Renders `track` over `range` into `destination` (a WAV, replaced if it
    // is there). Returns an error, or empty when the render has started --
    // then onDone hears how it ended, on the message thread: empty for a file
    // written, otherwise why not.
    juce::String start (te::AudioTrack& track, te::TimeRange range, const juce::File& destination,
                        std::function<void (const juce::String& error)> onDone);

    // Stops a running bounce and throws its file away. onDone is not called.
    void cancel();

    // True just before the render takes the transport, false once it has
    // handed it back, whichever way it ended.
    std::function<void (bool rendering)> onRenderingChanged;

    // 0..1 through the render, for whatever shows it.
    float getProgress() const  { return progress.load(); }

private:
    void timerCallback() override;
    void stop (bool deleteFile);

    te::Edit& edit;
    std::atomic<float> progress { 0.0f };
    juce::File destination;
    std::function<void (const juce::String&)> onDone;

    std::unique_ptr<te::Edit::ScopedRenderStatus> renderStatus;
    std::unique_ptr<te::Renderer::RenderTask> renderTask;
    std::unique_ptr<RenderThread> renderThread;
    sync::PrerollRender preroll;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackBouncer)
};

} // namespace carve::app
