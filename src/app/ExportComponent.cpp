#include "ExportComponent.h"
#include "Fonts.h"
#include "sync/EngineIds.h"

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

    // Sets the bit a render's tracksToDo uses for this track, which is its
    // index in the edit's track list. Written out rather than reached for
    // through tracktion's toBitSet, which ignores the array it is handed and
    // returns every track in the edit -- right for a mixdown, no use for one
    // stem.
    void setTrackBit (juce::BigInteger& bits, const juce::Array<te::Track*>& all, te::Track* track)
    {
        if (const auto index = all.indexOf (track); index >= 0)
            bits.setBit (index);
    }

    // The song's name, made into something a file system will take. The
    // fallback is what keeps an unnamed song from suggesting a file called
    // ".wav".
    juce::String fileStemFor (const model::Song& song)
    {
        const auto name = juce::File::createLegalFileName (song.getName()).trim();
        return name.isNotEmpty() ? name : juce::String ("Song");
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

    for (auto* label : { &contentLabel, &rangeLabel, &sampleRateLabel, &bitDepthLabel })
        styleLabel (*label, dimTextColour, fonts::small);

    contentLabel.setText ("Content", juce::dontSendNotification);
    rangeLabel.setText ("Range", juce::dontSendNotification);
    sampleRateLabel.setText ("Sample rate", juce::dontSendNotification);
    bitDepthLabel.setText ("Bit depth", juce::dontSendNotification);

    contentBox.addItem ("Mixdown", (int) Content::mixdown);
    contentBox.addItem ("Stems (one file per generator)", (int) Content::stems);
    contentBox.setSelectedId ((int) Content::mixdown, juce::dontSendNotification);
    contentBox.onChange = [this]
    {
        // The button says what the chooser is about to ask for: a file for a
        // mixdown, a folder for a set of stems.
        exportButton.setButtonText (isStemExport() ? "Export stems..." : "Export...");
        updateRangeSummary();
    };

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
             &headingLabel, &contentLabel, &rangeLabel, &sampleRateLabel, &bitDepthLabel,
             &contentBox, &rangeBox, &sampleRateBox, &bitDepthBox, &tailButton,
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
             &contentBox, &rangeBox, &sampleRateBox, &bitDepthBox, &tailButton, &exportButton })
        c->setEnabled (! rendering);

    closeButton.setButtonText (rendering ? "Cancel" : "Close");
    progressBar.setVisible (rendering);

    updateRangeSummary();
}

bool ExportComponent::isStemExport() const
{
    return contentBox.getSelectedId() == (int) Content::stems;
}

ExportComponent::StemPlan ExportComponent::planStems() const
{
    StemPlan plan;

    // Solo wins over mute, the way the mixer plays it: with anything soloed,
    // only the soloed generators are heard, so only those are worth a file.
    bool anySolo = false;

    for (auto track : te::getAudioTracks (edit))
        if (! sync::isBusTrack (*track) && track->isSolo (false))
            anySolo = true;

    for (auto track : te::getAudioTracks (edit))
    {
        if (sync::isReturnTrack (*track))
            continue;

        const bool isGroup = sync::isGroupTrack (*track);

        // A track inside a group is not a stem of its own. Its audio leaves
        // through the bus, and the render cannot take one member out of a
        // group: a bus pulls everything that feeds it, whatever it was asked
        // for. The group is the stem instead, which is what a group is.
        if (! isGroup && track->getOutput().getDestinationTrack() != nullptr)
            continue;

        ++plan.numStems;

        // A group is heard if it is not muted and something in it is being
        // heard -- which, with a solo somewhere, means a soloed member.
        const bool audible = [&]
        {
            if (track->isMuted (false))
                return false;

            if (! anySolo)
                return true;

            if (! isGroup)
                return track->isSolo (false);

            for (auto member : te::getAudioTracks (edit))
                if (member->getOutput().getDestinationTrack() == track && member->isSolo (false))
                    return true;

            return false;
        }();

        if (! audible)
            continue;

        // Numbered by position, so the folder lists in the order the mixer
        // shows -- and so two tracks sharing a name still get two files.
        const auto name = track->getName().trim();
        const auto fallback = (isGroup ? "Group " : "Track ") + juce::String (plan.numStems);

        plan.stems.push_back ({ track,
                                juce::String (plan.numStems).paddedLeft ('0', 2) + " "
                                    + juce::File::createLegalFileName (name.isNotEmpty() ? name : fallback) });
    }

    return plan;
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

    auto text = formatDuration (range.getStart().inSeconds())
                    + " - " + formatDuration (range.getEnd().inSeconds())
                    + "  (" + juce::String (seconds, 1) + " s, "
                    + juce::String (bitDepthBox.getSelectedId()) + " bit / "
                    + juce::String (sampleRateBox.getSelectedId() / 1000.0, 1) + " kHz)";

    if (isStemExport())
    {
        // How many files that is, and -- when the mixer is silencing some of
        // them -- out of how many, so a missing stem is answered here rather
        // than in the folder afterwards.
        const auto plan = planStems();
        const auto count = (int) plan.stems.size();

        text += "  -  " + juce::String (count);

        if (count < plan.numStems)
            text += " of " + juce::String (plan.numStems);

        text += count == 1 ? " stem" : " stems";
    }

    summaryLabel.setText (text, juce::dontSendNotification);
}

void ExportComponent::chooseDestinationAndRender()
{
    if (isRendering())
        return;

    if (isStemExport())
    {
        const auto suggested = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                   .getChildFile (fileStemFor (song) + " Stems");

        // A save panel rather than a folder picker, so the user names the
        // folder and we make it. Naming one that is already there is fine:
        // same-named stems are replaced, anything else in it is left alone.
        fileChooser = std::make_shared<juce::FileChooser> ("Export stems", suggested);
        fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                      | juce::FileBrowserComponent::canSelectDirectories,
                                  [safe = juce::Component::SafePointer (this)]
                                  (const juce::FileChooser& chooser)
        {
            if (safe == nullptr)
                return;

            const auto folder = chooser.getResult();

            if (folder == juce::File())
                return;

            safe->renderStemsTo (folder);
        });

        return;
    }

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
    Job job;
    job.destination = destination;
    job.label = destination.getFileName();

    // Every track, spelled out. Not the renderToFile overload that takes a
    // track array: that one wraps the render in a ScopedTrackSoloIsolator,
    // which unmutes everything it is asked to render, so a generator muted in
    // the mixer would still be audible in the file.
    job.tracks = te::toBitSet (te::getAllTracks (edit));

    stemFolder = juce::File();

    std::vector<Job> batch;
    batch.push_back (std::move (job));
    startJobs (std::move (batch));
}

void ExportComponent::renderStemsTo (const juce::File& folder)
{
    if (isRendering())
        return;

    const auto plan = planStems();

    if (plan.stems.empty())
    {
        setStatus (plan.numStems == 0 ? "There are no tracks to export"
                                      : "Everything is silenced - nothing to export",
                   errorColour);
        return;
    }

    if (! folder.createDirectory())
    {
        setStatus ("Could not create " + folder.getFullPathName(), errorColour);
        return;
    }

    auto* format = edit.engine.getAudioFileFormatManager().getDefaultFormat();
    const auto extension = format != nullptr && ! format->getFileExtensions().isEmpty()
                               ? format->getFileExtensions()[0]
                               : juce::String (".wav");

    // Appended rather than set through withFileExtension, which would cut a
    // generator called "Bass 2.0" back to "Bass 2".
    const auto suffix = extension.startsWithChar ('.') ? extension : "." + extension;

    // The return busses go into every stem. A track's share of the reverb is
    // part of that track, and nothing else is in the render to feed them --
    // an excluded track's send is not in the graph at all, so what comes back
    // is only ever this stem's. The master plugins stay out: they belong to
    // the mix, and would be applied a second time when the stems are summed.
    //
    // The group busses are not added the same way, and must not be: a bus
    // pulls in everything that feeds it whether or not the render asked for
    // those tracks, so one group in the list would put every track of that
    // group into every stem. A group is a stem in its own right instead (see
    // planStems).
    const auto allTracks = te::getAllTracks (edit);
    juce::BigInteger returnTracks;

    for (auto track : allTracks)
        if (sync::isReturnTrack (*track))
            setTrackBit (returnTracks, allTracks, track);

    std::vector<Job> batch;

    for (const auto& stem : plan.stems)
    {
        Job job;
        job.destination = folder.getChildFile (stem.name + suffix);
        job.label = stem.name;
        job.tracks = returnTracks;
        setTrackBit (job.tracks, allTracks, stem.track);
        job.includeMasterPlugins = false;

        // A generator with nothing to play writes a silent file rather than
        // failing the batch: an empty stem is still what that track sounds
        // like, and one of them must not cost the user the other eleven.
        job.requireAudio = false;

        batch.push_back (std::move (job));
    }

    stemFolder = folder;
    startJobs (std::move (batch));
}

void ExportComponent::startJobs (std::vector<Job> newJobs)
{
    if (isRendering() || newJobs.empty())
        return;

    if (edit.engine.getAudioFileFormatManager().getDefaultFormat() == nullptr)
    {
        setStatus ("No audio format available to write with", errorColour);
        return;
    }

    if (getTimeRange().getLength().inSeconds() <= 0.0)
    {
        setStatus ("Nothing to render", errorColour);
        return;
    }

    // Playback and rendering cannot both own the graph; ScopedRenderStatus
    // would drop the playback context anyway, this just leaves the transport
    // showing the truth.
    edit.getTransport().stop (false, false);

    jobs = std::move (newJobs);
    currentJob = 0;
    bytesWritten = 0;
    progressForBar = 0.0;

    // Held for the whole batch rather than per file -- a stem export would
    // otherwise hand the transport back and take it again between every one --
    // and destroyed on the message thread with reallocateOnDestruction set,
    // which is what gives the transport its playback context back afterwards.
    // (tracktion's own EditRenderer::render passes false here because it
    // destroys the object on the render thread.)
    renderStatus = std::make_unique<te::Edit::ScopedRenderStatus> (edit, true);

    if (! startCurrentJob())
        return;   // it has reported and torn down

    refreshOptions();
    startTimerHz (20);
}

bool ExportComponent::startCurrentJob()
{
    auto& engine = edit.engine;
    const auto& job = jobs[currentJob];

    job.destination.deleteFile();

    te::Renderer::Parameters params (edit);
    params.destFile = job.destination;
    params.audioFormat = engine.getAudioFileFormatManager().getDefaultFormat();
    params.bitDepth = bitDepthBox.getSelectedId();
    params.sampleRateForAudio = (double) sampleRateBox.getSelectedId();
    // The device's, so a render lands on the same block boundaries playback
    // did -- but not its zero: a device that never opened would otherwise
    // leave the render asking for blocks of nothing.
    const auto deviceBlockSize = engine.getDeviceManager().getBlockSize();
    params.blockSizeForAudio = deviceBlockSize > 0 ? deviceBlockSize : 512;
    params.time = getTimeRange();
    params.usePlugins = true;
    params.useMasterPlugins = job.includeMasterPlugins;
    params.checkNodesForAudio = job.requireAudio;
    params.tracksToDo = job.tracks;

    progress = 0.0f;

    // Builds the playback graph, on this (the message) thread.
    renderTask = te::render_utils::createRenderTask (params, "Export", &progress, nullptr);

    if (renderTask == nullptr)
    {
        setStatus ("Could not build a render graph for " + job.label, errorColour);
        stopRender (true);
        return false;
    }

    renderThread = std::make_unique<RenderThread> (*renderTask);
    renderThread->startThread (juce::Thread::Priority::normal);

    setStatus (jobs.size() > 1
                   ? "Rendering " + job.label + "  (" + juce::String ((int) currentJob + 1)
                         + " of " + juce::String ((int) jobs.size()) + ")..."
                   : "Rendering " + job.label + "...",
               dimTextColour);
    return true;
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

    // Only the file that was being written: the stems already finished are
    // whole, and are what they would have been had the batch run to the end.
    if (deletePartialFile && currentJob < jobs.size())
        jobs[currentJob].destination.deleteFile();

    progressForBar = 0.0;
    refreshOptions();
}

void ExportComponent::timerCallback()
{
    // One bar for the batch: the files already written, plus how far into
    // this one the render has got.
    const auto total = (double) juce::jmax ((size_t) 1, jobs.size());
    progressForBar = ((double) currentJob + (double) progress.load()) / total;
    progressBar.repaint();

    if (renderThread != nullptr && renderThread->hasFinished())
        renderFinished();
}

void ExportComponent::renderFinished()
{
    // Read before stopRender destroys the task.
    const auto error = renderTask != nullptr ? renderTask->errorMessage : juce::String();
    const auto file = currentJob < jobs.size() ? jobs[currentJob].destination : juce::File();
    const auto label = currentJob < jobs.size() ? jobs[currentJob].label : juce::String();

    if (error.isNotEmpty())
    {
        stopRender (true);
        setStatus ("Render failed: " + error, errorColour);
        return;
    }

    if (! file.existsAsFile())
    {
        stopRender (false);
        setStatus ("Render produced no file for " + label, errorColour);
        return;
    }

    bytesWritten += file.getSize();

    if (++currentJob < jobs.size())
    {
        // Between files the thread and the task go and the render status
        // stays, so the next stem starts on the graph this one just used.
        renderThread.reset();
        renderTask.reset();

        if (! startCurrentJob())
            return;

        return;
    }

    const auto count = (int) jobs.size();
    const auto folder = stemFolder;
    const auto total = juce::File::descriptionOfSizeInBytes (bytesWritten);

    stopRender (false);

    if (folder != juce::File())
        setStatus ("Exported " + juce::String (count) + (count == 1 ? " stem to " : " stems to ")
                       + folder.getFullPathName() + "  (" + total + ")",
                   okColour);
    else
        setStatus ("Exported " + file.getFullPathName() + "  (" + total + ")", okColour);
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

    layoutRow (contentLabel, contentBox);
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
