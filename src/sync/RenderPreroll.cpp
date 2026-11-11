#include "RenderPreroll.h"

#include "plugins/Loudness.h"

#include "VorbisComments.h"

namespace carve::sync
{

juce::int64 getPrerollSamples (int blockSize, double sampleRate)
{
    const auto block = (juce::int64) juce::jmax (1, blockSize);
    const auto wanted = juce::jmax ((juce::int64) std::ceil (sampleRate * 0.5), 2 * block);
    return (wanted + block - 1) / block * block;
}

PrerollRender applyPreroll (te::Renderer::Parameters& params, te::Engine& engine)
{
    PrerollRender render;
    render.destination = params.destFile;
    render.prerollSamples = getPrerollSamples (params.blockSizeForAudio, params.sampleRateForAudio);

    // In the temporary folder, not beside the destination. It was beside it
    // once, but inside the App Sandbox a destination picked in a save panel
    // is the one path the app may write: a second file next to it is
    // refused. Nothing needs the two on one volume -- the result is copied
    // across sample by sample, not moved.
    render.renderFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("carve-" + render.destination.getFileNameWithoutExtension()
                                               + ".rendering.wav")
                            .getNonexistentSibling (false);

    params.destFile = render.renderFile;
    params.audioFormat = engine.getAudioFileFormatManager().getWavFormat();
    params.bitDepth = 32;   // float
    params.time = params.time.withStart (params.time.getStart()
                                           - te::TimeDuration::fromSamples (render.prerollSamples,
                                                                            params.sampleRateForAudio));
    return render;
}

juce::String finishPreroll (const PrerollRender& render, te::Engine& engine, const RenderOutput& output,
                            std::function<bool()> shouldStop)
{
    auto& formats = engine.getAudioFileFormatManager();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.getWavFormat()->createReaderFor (
        render.renderFile.createInputStream().release(), true));

    if (reader == nullptr)
    {
        discardPreroll (render);
        return "could not read the rendered audio back";
    }

    const auto available = juce::jmax ((juce::int64) 0, reader->lengthInSamples - render.prerollSamples);
    const auto keep = output.exactLength >= 0 ? output.exactLength : available;
    const auto numChannels = (int) reader->numChannels;
    const auto sampleRate = reader->sampleRate;

    auto* format = output.format != nullptr ? output.format : formats.getWavFormat();

    render.destination.deleteFile();
    std::unique_ptr<juce::OutputStream> out (render.destination.createOutputStream());

    if (out == nullptr)
    {
        discardPreroll (render);
        return "could not write " + render.destination.getFileName();
    }

    // What the renderer stamped on its file -- the BWF chunk, with the time
    // the render starts at -- carried over, so the destination has it as a
    // direct render would. That start is the preroll's, earlier than the
    // range by exactly what is being cut, so it moves forward by the same.
    // Then whatever the caller asked for on top.
    auto metadata = reader->metadataValues;

    if (const auto key = juce::String (juce::WavAudioFormat::bwavTimeReference); metadata.containsKey (key))
        metadata.set (key, juce::String (juce::jmax ((juce::int64) 0,
                                                     metadata[key].getLargeIntValue() + render.prerollSamples)));

    metadata.addArray (output.metadata);

    std::unique_ptr<juce::AudioFormatWriter> writer (
        format->createWriterFor (out.get(), sampleRate, (unsigned int) numChannels,
                                 output.bitDepth, metadata, output.qualityIndex));

    if (writer == nullptr)
    {
        out.reset();
        discardPreroll (render);
        render.destination.deleteFile();
        return "could not write " + format->getFormatName() + " at these settings";
    }

    out.release();   // the writer owns it now

    constexpr int chunk = 65536;
    juce::AudioBuffer<float> buffer (numChannels, chunk);
    bool ok = true;

    // Measure first: the loudness and the true peak of what was rendered,
    // which normalising works from and the report gives back.
    double gain = 1.0;

    if (output.report != nullptr || std::isfinite (output.loudnessTarget))
    {
        plugins::LoudnessMeter loudness;
        plugins::TruePeakMeter truePeak;
        loudness.prepare (sampleRate);

        for (juce::int64 done = 0; done < keep;)
        {
            if (shouldStop && shouldStop())
                break;

            const auto n = (int) juce::jmin ((juce::int64) chunk, keep - done);
            reader->read (&buffer, 0, n, render.prerollSamples + done, true, true);
            loudness.process (buffer.getArrayOfReadPointers(), numChannels, n);
            truePeak.process (buffer.getArrayOfReadPointers(), numChannels, n);
            done += n;
        }

        const auto measured = loudness.integrated();
        const auto peak = truePeak.getPeakDb();
        double gainDb = 0.0;
        bool held = false;

        if (std::isfinite (output.loudnessTarget) && std::isfinite (measured))
        {
            gainDb = output.loudnessTarget - measured;

            if (peak + gainDb > output.truePeakCeilingDb)
            {
                gainDb = output.truePeakCeilingDb - peak;
                held = true;
            }

            gain = juce::Decibels::decibelsToGain (gainDb);
        }

        if (output.report != nullptr)
        {
            output.report->lufsAsRendered = measured;
            output.report->lufs = measured + gainDb;
            output.report->truePeakDb = peak + gainDb;
            output.report->gainDb = gainDb;
            output.report->heldByCeiling = held;
        }
    }

    // TPDF dither: two uniform randoms a least significant bit wide, summed.
    const bool dither = output.dither && output.bitDepth > 0 && output.bitDepth < 32
                         && (output.format == nullptr || output.format == formats.getWavFormat());
    const auto lsb = dither ? 1.0f / (float) (1 << (output.bitDepth - 1)) : 0.0f;
    juce::Random random (0x5eed);   // the same noise every time: a render is reproducible

    for (juce::int64 done = 0; done < keep && ok;)
    {
        if (shouldStop && shouldStop())
        {
            writer.reset();
            reader.reset();
            discardPreroll (render);
            render.destination.deleteFile();
            return "cancelled";
        }

        const auto n = (int) juce::jmin ((juce::int64) chunk, keep - done);

        // Past the end of what was rendered -- only when an exact length asks
        // for a sample or so more than the render rounded to -- reads as
        // silence, which is what the song has there.
        ok = reader->read (&buffer, 0, n, render.prerollSamples + done, true, true);

        if (ok && gain != 1.0)
            buffer.applyGain (0, n, (float) gain);

        if (ok && dither)
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);

                for (int i = 0; i < n; ++i)
                    d[i] += (random.nextFloat() - random.nextFloat()) * lsb;
            }

        ok = ok && writer->writeFromAudioSampleBuffer (buffer, 0, n);
        done += n;
    }

    writer.reset();
    reader.reset();
    render.renderFile.deleteFile();

    if (! ok)
    {
        render.destination.deleteFile();
        return "could not write " + render.destination.getFileName();
    }

    // JUCE's Ogg writer has a fixed list of comments it will write and drops
    // the rest, the game loop's among them, so those go in now that the file
    // is closed. A game loop without its loop points is not what was asked
    // for, so failing here fails the export.
    if (dynamic_cast<juce::OggVorbisAudioFormat*> (format) != nullptr && output.metadata.size() > 0)
    {
        if (const auto error = setVorbisComments (render.destination, output.metadata); error.isNotEmpty())
        {
            render.destination.deleteFile();
            return error;
        }
    }

    return {};
}

void discardPreroll (const PrerollRender& render)
{
    render.renderFile.deleteFile();
}

//==============================================================================
std::optional<GameLoop> getGameLoop (const model::Song& song, const te::Edit& edit, double sampleRate)
{
    if (! song.hasLoopRange())
        return std::nullopt;

    const auto start = edit.tempoSequence.toTime (te::BeatPosition::fromBeats (song.getLoopStart()));
    const auto end = edit.tempoSequence.toTime (te::BeatPosition::fromBeats (song.getLoopEnd()));

    GameLoop loop;
    loop.range = { te::TimePosition(), end };
    loop.loopStart = te::toSamples (start, sampleRate);
    loop.loopLength = te::toSamples (end, sampleRate) - loop.loopStart;

    if (loop.loopLength <= 0)
        return std::nullopt;

    return loop;
}

juce::StringPairArray gameLoopMetadata (const GameLoop& loop)
{
    juce::StringPairArray metadata;
    metadata.set ("LOOPSTART", juce::String (loop.loopStart));
    metadata.set ("LOOPLENGTH", juce::String (loop.loopLength));
    return metadata;
}

} // namespace carve::sync
