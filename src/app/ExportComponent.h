#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::app
{

// Mixdown to an audio file: pick a range, a sample rate and a bit depth, choose
// a destination, watch a progress bar.
//
// The render runs on a thread of our own rather than through the engine's
// UIBehaviour::runTaskWithProgressBar. Two reasons, and the second is the one
// that matters:
//
//  - the GUI builds its Engine with the default UIBehaviour, whose
//    runTaskWithProgressBar is a jassertfalse that does nothing, so a render
//    handed to it would silently produce an empty file (the console renderer
//    only works because it installs HeadlessUIBehaviour to run the job inline);
//  - RenderTask's first block builds its NodeRenderContext through
//    tracktion's callBlocking, which posts to the message thread and waits.
//    Running the task on the message thread -- inline, or under a modal loop --
//    would deadlock or freeze the app. It has to be a real background thread
//    with the message loop left free to serve it.
//
// Sized to its content -- put it in a window of preferredWidth x preferredHeight.
class ExportComponent : public juce::Component,
                        private juce::Timer
{
public:
    ExportComponent (te::Edit&, model::Song);
    ~ExportComponent() override;

    static constexpr int preferredWidth = 400;
    static constexpr int preferredHeight = 268;

    // The song is where the loop range and the default file name come from;
    // the Edit only knows about seconds.
    void setSong (model::Song);

    bool isRendering() const  { return renderThread != nullptr; }

    // Renders the current options to this file, replacing whatever is there.
    // Returns immediately; the render runs on its own thread. Public because
    // it is the whole of what this component does -- the file chooser is just
    // one way of arriving at a destination.
    void renderTo (const juce::File& destination);

    // Stops a running render and throws the half-written file away. Safe to
    // call when nothing is rendering, and safe to call before destroying this.
    void cancelRender();

    // The dialog is done with -- the owner should destroy the window. Never
    // called from inside a render; cancel first.
    std::function<void()> onFinished;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // Runs RenderTask::runJob until it says it is finished. Nothing else
    // touches the task while this is alive.
    class RenderThread;

    enum class Range { wholeSong = 1, loopRange = 2 };

    void refreshOptions();
    void updateRangeSummary();

    // The range to render, in edit time. Empty when there is nothing to render.
    te::TimeRange getTimeRange() const;

    void chooseDestinationAndRender();

    // Tears the render down in the order the engine needs: the thread first
    // (it owns the task's execution), then the task, then the render status --
    // which is what puts the transport back. Message thread only.
    void stopRender (bool deletePartialFile);

    void timerCallback() override;
    void renderFinished();

    void setStatus (const juce::String& text, juce::Colour colour);

    static constexpr int labelColumnWidth = 92;

    te::Edit& edit;
    model::Song song;

    // Written by the render thread, read by the timer, which copies it into
    // the double the bar watches. Declared before the bar: ProgressBar reads
    // the value it is given in its constructor.
    std::atomic<float> progress { 0.0f };
    double progressForBar = 0.0;

    juce::Label headingLabel, rangeLabel, sampleRateLabel, bitDepthLabel;
    juce::ComboBox rangeBox, sampleRateBox, bitDepthBox;
    juce::ToggleButton tailButton { "Add a 1 second tail" };
    juce::Label summaryLabel, statusLabel;
    juce::ProgressBar progressBar { progressForBar };
    juce::TextButton exportButton { "Export..." }, closeButton { "Close" };

    std::shared_ptr<juce::FileChooser> fileChooser;

    juce::File destinationFile;

    // Declared in destruction order: renderStatus outlives the task and the
    // thread, so the transport is only reallocated once nothing is rendering.
    std::unique_ptr<te::Edit::ScopedRenderStatus> renderStatus;
    std::unique_ptr<te::Renderer::RenderTask> renderTask;
    std::unique_ptr<RenderThread> renderThread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportComponent)
};

} // namespace carve::app
