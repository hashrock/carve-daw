#pragma once

#include <atomic>
#include <functional>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::app
{

//==============================================================================
// A render on a thread of its own: RenderTask::runJob does one block per call
// and reports jobHasFinished when the file is complete. The export and a
// bounce both run theirs here.
//
// Cancelling is safe even while the task is inside tracktion's callBlocking
// (which it is for the first block, building the playback graph): that wait
// polls juce::Thread::threadShouldExit(), so signalling this thread unblocks it
// within 50ms instead of deadlocking against a message thread that is waiting
// for us.
class RenderThread : public juce::Thread
{
public:
    // afterRender runs here, on this thread, once the task has finished
    // without an error or a cancel: the trim of the early start (see
    // RenderPreroll.h), which on a long render is too much copying for the
    // message thread. It is handed a "should I stop" to poll.
    RenderThread (te::Renderer::RenderTask& taskToRun,
                  std::function<juce::String (std::function<bool()>)> afterRenderToRun)
        : juce::Thread ("Carve Render"), task (taskToRun), afterRender (std::move (afterRenderToRun))
    {
    }

    ~RenderThread() override
    {
        stopThread (5000);
    }

    bool hasFinished() const  { return finished.load(); }

    // What afterRender reported. Only read once hasFinished() is true.
    juce::String getAfterRenderError() const  { return afterRenderError; }

    void run() override
    {
        bool rendered = false;

        while (! threadShouldExit())
            if (task.runJob() == juce::ThreadPoolJob::jobHasFinished)
            {
                rendered = true;
                break;
            }

        if (rendered && task.errorMessage.isEmpty() && afterRender)
            afterRenderError = afterRender ([this] { return threadShouldExit(); });

        finished = true;
    }

private:
    te::Renderer::RenderTask& task;
    std::function<juce::String (std::function<bool()>)> afterRender;
    juce::String afterRenderError;
    std::atomic<bool> finished { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RenderThread)
};

} // namespace carve::app
