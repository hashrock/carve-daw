#include "ExportComponent.h"
#include "Fonts.h"

namespace carve::app
{

namespace
{
    const juce::Colour backgroundColour { 0xff232327 };
    const juce::Colour textColour { 0xffd8d8dc };
    const juce::Colour dimTextColour { 0xff9a9aa4 };
    const juce::Colour okColour { 0xff4fc27a };
    const juce::Colour errorColour { 0xffe0704f };

    // Offered when the format allows them. Anything above this is a niche
    // choice that only makes the menu longer.
    constexpr int candidateSampleRates[] = { 44100, 48000, 88200, 96000 };
    constexpr int candidateBitDepths[] = { 16, 24, 32 };

    juce::String formatDuration (double seconds)
    {
        const auto total = (int) std::floor (seconds);
        return juce::String::formatted ("%d:%02d", total / 60, total % 60);
    }

    void styleLabel (juce::Label& label, juce::Colour colour, float height)
    {
        label.setFont (uiFont (height));
        label.setColour (juce::Label::textColourId, colour);
    }
} // namespace

//==============================================================================
// The render itself. RenderTask::runJob does one block per call and reports
// jobHasFinished when the file is complete.
//
// Cancelling is safe even while the task is inside tracktion's callBlocking
// (which it is for the first block, building the playback graph): that wait
// polls juce::Thread::threadShouldExit(), so signalling this thread unblocks it
// within 50ms instead of deadlocking against a message thread that is waiting
// for us.
class ExportComponent::RenderThread : public juce::Thread
{
public:
    explicit RenderThread (te::Renderer::RenderTask& taskToRun)
        : juce::Thread ("Carve Export"), task (taskToRun)
    {
    }

    ~RenderThread() override
    {
        stopThread (5000);
    }

    bool hasFinished() const  { return finished.load(); }

    void run() override
    {
        while (! threadShouldExit())
            if (task.runJob() == juce::ThreadPoolJob::jobHasFinished)
                break;

        finished = true;
    }

private:
    te::Renderer::RenderTask& task;
    std::atomic<bool> finished { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RenderThread)
};

//==============================================================================
ExportComponent::ExportComponent (te::Edit& editToRender, model::Song songToRender)
    : edit (editToRender), song (std::move (songToRender))
{
    styleLabel (headingLabel, textColour, fonts::title);
    headingLabel.setText ("Export Audio", juce::dontSendNotification);

    for (auto* label : { &rangeLabel, &sampleRateLabel, &bitDepthLabel })
        styleLabel (*label, dimTextColour, fonts::small);

    rangeLabel.setText ("Range", juce::dontSendNotification);
    sampleRateLabel.setText ("Sample rate", juce::dontSendNotification);
    bitDepthLabel.setText ("Bit depth", juce::dontSendNotification);

    rangeBox.addItem ("Whole song", (int) Range::wholeSong);
    rangeBox.addItem ("Loop range", (int) Range::loopRange);
    rangeBox.setSelectedId ((int) Range::wholeSong, juce::dontSendNotification);
    rangeBox.onChange = [this]
    {
        // A whole-song mixdown wants the reverb to ring out; a loop export is
        // meant to loop, and a tail would stop it butting up against itself.
        tailButton.setToggleState (rangeBox.getSelectedId() == (int) Range::wholeSong,
                                   juce::dontSendNotification);
        updateRangeSummary();
    };

    auto* format = edit.engine.getAudioFileFormatManager().getDefaultFormat();
    const auto possibleRates = format != nullptr ? format->getPossibleSampleRates()
                                                 : juce::Array<int>();
    const auto possibleDepths = format != nullptr ? format->getPossibleBitDepths()
                                                  : juce::Array<int>();

    for (auto rate : candidateSampleRates)
        if (possibleRates.isEmpty() || possibleRates.contains (rate))
            sampleRateBox.addItem (juce::String (rate) + " Hz", rate);

    for (auto depth : candidateBitDepths)
        if (possibleDepths.isEmpty() || possibleDepths.contains (depth))
            bitDepthBox.addItem (juce::String (depth) + " bit", depth);

    // Default to what the app is playing at, so an export matches what was
    // heard unless the user asks for something else.
    const auto deviceRate = (int) edit.engine.getDeviceManager().getSampleRate();
    sampleRateBox.setSelectedId (sampleRateBox.indexOfItemId (deviceRate) >= 0 ? deviceRate : 44100,
                                 juce::dontSendNotification);
    bitDepthBox.setSelectedId (24, juce::dontSendNotification);

    sampleRateBox.onChange = [this] { updateRangeSummary(); };
    bitDepthBox.onChange = [this] { updateRangeSummary(); };
    tailButton.setToggleState (true, juce::dontSendNotification);
    tailButton.setColour (juce::ToggleButton::textColourId, dimTextColour);
    tailButton.onClick = [this] { updateRangeSummary(); };

    styleLabel (summaryLabel, dimTextColour, fonts::small);
    styleLabel (statusLabel, dimTextColour, fonts::small);

    progressBar.setVisible (false);

    exportButton.onClick = [this] { chooseDestinationAndRender(); };
    closeButton.onClick = [this]
    {
        if (isRendering())
        {
            // Cancel, but stay open: the user gets told what happened.
            cancelRender();
            return;
        }

        if (onFinished != nullptr)
            onFinished();
    };

    for (auto* c : std::initializer_list<juce::Component*> {
             &headingLabel, &rangeLabel, &sampleRateLabel, &bitDepthLabel,
             &rangeBox, &sampleRateBox, &bitDepthBox, &tailButton,
             &summaryLabel, &statusLabel, &progressBar, &exportButton, &closeButton })
        addAndMakeVisible (c);

    setSize (preferredWidth, preferredHeight);
    refreshOptions();
}

ExportComponent::~ExportComponent()
{
    // Leaves any half-written file behind rather than deleting something the
    // user may have been watching appear; the cancel button is the tidy path.
    stopRender (false);
}

void ExportComponent::cancelRender()
{
    if (! isRendering())
        return;

    stopRender (true);
    setStatus ("Cancelled", dimTextColour);
}

void ExportComponent::setSong (model::Song newSong)
{
    song = std::move (newSong);
    refreshOptions();
}

void ExportComponent::refreshOptions()
{
    const bool rendering = isRendering();
    const bool hasLoop = song.hasLoopRange();

    // Falling back rather than leaving a range selected that no longer exists.
    if (! hasLoop && rangeBox.getSelectedId() == (int) Range::loopRange)
        rangeBox.setSelectedId ((int) Range::wholeSong, juce::dontSendNotification);

    rangeBox.setItemEnabled ((int) Range::loopRange, hasLoop);

    for (auto* c : std::initializer_list<juce::Component*> {
             &rangeBox, &sampleRateBox, &bitDepthBox, &tailButton, &exportButton })
        c->setEnabled (! rendering);

    closeButton.setButtonText (rendering ? "Cancel" : "Close");
    progressBar.setVisible (rendering);

    updateRangeSummary();
}

te::TimeRange ExportComponent::getTimeRange() const
{
    const auto tail = tailButton.getToggleState() ? 1.0 : 0.0;

    if (rangeBox.getSelectedId() == (int) Range::loopRange && song.hasLoopRange())
    {
        const auto beats = te::BeatRange (te::BeatPosition::fromBeats (song.getLoopStart()),
                                          te::BeatPosition::fromBeats (song.getLoopEnd()));
        const auto time = edit.tempoSequence.toTime (beats);

        return { time.getStart(),
                 te::TimePosition::fromSeconds (time.getEnd().inSeconds() + tail) };
    }

    return { te::TimePosition(),
             te::TimePosition::fromSeconds (edit.getLength().inSeconds() + tail) };
}

void ExportComponent::updateRangeSummary()
{
    const auto range = getTimeRange();
    const auto seconds = range.getLength().inSeconds();

    if (seconds <= 0.0)
    {
        summaryLabel.setText ("Nothing to render - the song is empty",
                              juce::dontSendNotification);
        return;
    }

    summaryLabel.setText (formatDuration (range.getStart().inSeconds())
                              + " - " + formatDuration (range.getEnd().inSeconds())
                              + "  (" + juce::String (seconds, 1) + " s, "
                              + juce::String (bitDepthBox.getSelectedId()) + " bit / "
                              + juce::String (sampleRateBox.getSelectedId() / 1000.0, 1) + " kHz)",
                          juce::dontSendNotification);
}

void ExportComponent::chooseDestinationAndRender()
{
    if (isRendering())
        return;

    auto* format = edit.engine.getAudioFileFormatManager().getDefaultFormat();
    const auto extension = format != nullptr && ! format->getFileExtensions().isEmpty()
                               ? format->getFileExtensions()[0]
                               : juce::String (".wav");

    const auto suggested = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                               .getChildFile (juce::File::createLegalFileName (song.getName()))
                               .withFileExtension (extension);

    fileChooser = std::make_shared<juce::FileChooser> ("Export audio", suggested, "*" + extension);
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe = juce::Component::SafePointer (this), extension]
                              (const juce::FileChooser& chooser)
    {
        if (safe == nullptr)
            return;

        const auto file = chooser.getResult();

        if (file == juce::File())
            return;

        safe->renderTo (file.withFileExtension (extension));
    });
}

void ExportComponent::renderTo (const juce::File& destination)
{
    auto& engine = edit.engine;
    auto* format = engine.getAudioFileFormatManager().getDefaultFormat();

    if (format == nullptr)
    {
        setStatus ("No audio format available to write with", errorColour);
        return;
    }

    const auto range = getTimeRange();

    if (range.getLength().inSeconds() <= 0.0)
    {
        setStatus ("Nothing to render", errorColour);
        return;
    }

    // Playback and rendering cannot both own the graph; ScopedRenderStatus
    // would drop the playback context anyway, this just leaves the transport
    // showing the truth.
    edit.getTransport().stop (false, false);

    destinationFile = destination;
    destinationFile.deleteFile();

    te::Renderer::Parameters params (edit);
    params.destFile = destinationFile;
    params.audioFormat = format;
    params.bitDepth = bitDepthBox.getSelectedId();
    params.sampleRateForAudio = (double) sampleRateBox.getSelectedId();
    params.blockSizeForAudio = engine.getDeviceManager().getBlockSize();
    params.time = range;
    params.usePlugins = true;
    params.useMasterPlugins = true;

    // Every track, spelled out. Not the renderToFile overload that takes a
    // track array: that one wraps the render in a ScopedTrackSoloIsolator,
    // which unmutes everything it is asked to render, so a generator muted in
    // the mixer would still be audible in the file.
    params.tracksToDo = te::toBitSet (te::getAllTracks (edit));

    // Held for as long as the render runs, and destroyed on the message thread
    // with reallocateOnDestruction set, which is what gives the transport its
    // playback context back afterwards. (tracktion's own EditRenderer::render
    // passes false here because it destroys the object on the render thread.)
    renderStatus = std::make_unique<te::Edit::ScopedRenderStatus> (edit, true);

    progress = 0.0f;
    progressForBar = 0.0;

    // Builds the playback graph, on this (the message) thread.
    renderTask = te::render_utils::createRenderTask (params, "Export", &progress, nullptr);

    if (renderTask == nullptr)
    {
        stopRender (true);
        setStatus ("Could not build a render graph for this song", errorColour);
        return;
    }

    renderThread = std::make_unique<RenderThread> (*renderTask);
    renderThread->startThread (juce::Thread::Priority::normal);

    setStatus ("Rendering " + destinationFile.getFileName() + "...", dimTextColour);
    refreshOptions();
    startTimerHz (20);
}

void ExportComponent::stopRender (bool deletePartialFile)
{
    stopTimer();

    // Order matters: the thread is the only thing running the task, and the
    // render status has to outlive both so the transport is not reallocated
    // into a graph that is still being rendered.
    renderThread.reset();
    renderTask.reset();
    renderStatus.reset();

    if (deletePartialFile && destinationFile != juce::File())
        destinationFile.deleteFile();

    progressForBar = 0.0;
    refreshOptions();
}

void ExportComponent::timerCallback()
{
    progressForBar = (double) progress.load();
    progressBar.repaint();

    if (renderThread != nullptr && renderThread->hasFinished())
        renderFinished();
}

void ExportComponent::renderFinished()
{
    // Read before stopRender destroys the task.
    const auto error = renderTask != nullptr ? renderTask->errorMessage : juce::String();
    const auto file = destinationFile;

    stopRender (error.isNotEmpty());

    if (error.isNotEmpty())
    {
        setStatus ("Render failed: " + error, errorColour);
        return;
    }

    if (! file.existsAsFile())
    {
        setStatus ("Render produced no file", errorColour);
        return;
    }

    setStatus ("Exported " + file.getFullPathName() + "  ("
                   + juce::File::descriptionOfSizeInBytes (file.getSize()) + ")",
               okColour);
}

void ExportComponent::setStatus (const juce::String& text, juce::Colour colour)
{
    statusLabel.setColour (juce::Label::textColourId, colour);
    statusLabel.setText (text, juce::dontSendNotification);
}

void ExportComponent::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);
}

void ExportComponent::resized()
{
    auto area = getLocalBounds().reduced (14, 12);

    headingLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);

    // label column | control column
    const auto layoutRow = [&area] (juce::Label& label, juce::Component& control)
    {
        auto row = area.removeFromTop (24);
        label.setBounds (row.removeFromLeft (labelColumnWidth));
        control.setBounds (row);
        area.removeFromTop (6);
    };

    layoutRow (rangeLabel, rangeBox);
    layoutRow (sampleRateLabel, sampleRateBox);
    layoutRow (bitDepthLabel, bitDepthBox);

    tailButton.setBounds (area.removeFromTop (22).withTrimmedLeft (labelColumnWidth));
    area.removeFromTop (4);
    summaryLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (4);

    auto buttons = area.removeFromBottom (26);
    exportButton.setBounds (buttons.removeFromRight (96));
    buttons.removeFromRight (8);
    closeButton.setBounds (buttons.removeFromRight (76));
    area.removeFromBottom (8);

    progressBar.setBounds (area.removeFromTop (16));
    area.removeFromTop (6);
    statusLabel.setBounds (area);
}

} // namespace carve::app
