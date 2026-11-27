#include "TrackBouncer.h"

#include "sync/EditSync.h"
#include "sync/EngineIds.h"

namespace carve::app
{

TrackBouncer::TrackBouncer (te::Edit& editToRender)
    : edit (editToRender)
{
}

TrackBouncer::~TrackBouncer()
{
    onRenderingChanged = nullptr;   // the owner may be going away too
    cancel();
}

juce::String TrackBouncer::start (te::AudioTrack& track, te::TimeRange range, const juce::File& file,
                                  std::function<void (const juce::String&)> whenDone)
{
    if (isBouncing())
        return "A bounce is already running";

    if (range.getLength().inSeconds() <= 0.0)
        return "Nothing to bounce - the range is empty";

    if (track.getOutput().getDestinationTrack() != nullptr)
        return "A track inside a group can't be bounced on its own - bounce the group's members out of it first";

    if (track.isMuted (false))
        return "The track is muted";

    auto& engine = edit.engine;

    if (engine.getAudioFileFormatManager().getDefaultFormat() == nullptr)
        return "No audio format available to write with";

    if (! file.getParentDirectory().createDirectory())
        return "Could not create " + file.getParentDirectory().getFullPathName();

    // The track and the return busses, which bring its sends back with it;
    // nothing else is in the graph to feed them. Bit index is position in
    // the Edit's track list (see ExportComponent's setTrackBit).
    const auto allTracks = te::getAllTracks (edit);
    juce::BigInteger tracks;

    for (int i = 0; i < allTracks.size(); ++i)
        if (allTracks[i] == &track || sync::isReturnTrack (*allTracks[i]))
            tracks.setBit (i);

    edit.getTransport().stop (false, false);

    if (onRenderingChanged)
        onRenderingChanged (true);

    // A singer's notes are synthesised on worker threads; a render started
    // before they are in has holes where they go.
    sync::waitForVoiceRenders (edit, 300000);

    destination = file;
    destination.deleteFile();
    onDone = std::move (whenDone);
    progress = 0.0f;

    renderStatus = std::make_unique<te::Edit::ScopedRenderStatus> (edit, true);

    te::Renderer::Parameters params (edit);
    params.destFile = destination;
    params.audioFormat = engine.getAudioFileFormatManager().getDefaultFormat();
    params.bitDepth = 24;

    const auto deviceRate = engine.getDeviceManager().getSampleRate();
    params.sampleRateForAudio = deviceRate > 0.0 ? deviceRate : 44100.0;

    const auto deviceBlockSize = engine.getDeviceManager().getBlockSize();
    params.blockSizeForAudio = deviceBlockSize > 0 ? deviceBlockSize : 512;
    params.time = range;
    params.usePlugins = true;
    params.useMasterPlugins = false;
    params.checkNodesForAudio = true;   // a silent bounce is a mistake, not a loop
    params.tracksToDo = tracks;

    preroll = sync::applyPreroll (params, engine);
    renderTask = te::render_utils::createRenderTask (params, "Bounce", &progress, nullptr);

    if (renderTask == nullptr)
    {
        stop (true);
        return "Could not build a render graph for the track";
    }

    sync::RenderOutput output;
    output.bitDepth = 24;

    renderThread = std::make_unique<RenderThread> (*renderTask,
        [&engine, render = preroll, output] (std::function<bool()> shouldStop)
        {
            return sync::finishPreroll (render, engine, output, std::move (shouldStop));
        });
    renderThread->startThread (juce::Thread::Priority::normal);

    startTimerHz (20);
    return {};
}

void TrackBouncer::cancel()
{
    if (isBouncing() || renderStatus != nullptr)
        stop (true);
}

void TrackBouncer::stop (bool deleteFile)
{
    stopTimer();

    // The thread first (it is what runs the task), then the task, then the
    // render status, which hands the transport back.
    renderThread.reset();
    renderTask.reset();
    renderStatus.reset();

    if (deleteFile)
        destination.deleteFile();

    sync::discardPreroll (preroll);

    if (onRenderingChanged)
        onRenderingChanged (false);
}

void TrackBouncer::timerCallback()
{
    if (renderThread == nullptr || ! renderThread->hasFinished())
        return;

    auto error = renderTask != nullptr ? renderTask->errorMessage : juce::String();

    if (error.isEmpty())
        error = renderThread->getAfterRenderError();

    if (error.isEmpty() && ! destination.existsAsFile())
        error = "The render produced no file";

    stop (error.isNotEmpty());

    if (auto done = std::exchange (onDone, nullptr))
        done (error);
}

} // namespace carve::app
