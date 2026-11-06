#include "RenderPreroll.h"

namespace carve::sync
{

juce::int64 getPrerollSamples (int blockSize, double sampleRate)
{
    const auto block = (juce::int64) juce::jmax (1, blockSize);
    const auto wanted = juce::jmax ((juce::int64) std::ceil (sampleRate * 0.5), 2 * block);
    return (wanted + block - 1) / block * block;
}

PrerollRender applyPreroll (te::Renderer::Parameters& params)
{
    PrerollRender render;
    render.destination = params.destFile;
    render.prerollSamples = getPrerollSamples (params.blockSizeForAudio, params.sampleRateForAudio);

    // Beside the destination, so the copy at the end stays on one volume, and
    // named so a crash mid-render leaves something recognisable.
    render.renderFile = render.destination.getSiblingFile ("." + render.destination.getFileNameWithoutExtension()
                                                               + ".rendering"
                                                               + render.destination.getFileExtension())
                            .getNonexistentSibling (false);

    params.destFile = render.renderFile;
    params.time = params.time.withStart (params.time.getStart()
                                           - te::TimeDuration::fromSamples (render.prerollSamples,
                                                                            params.sampleRateForAudio));
    return render;
}

juce::String finishPreroll (const PrerollRender& render, te::Engine& engine, int bitDepth,
                            std::function<bool()> shouldStop)
{
    auto& formats = engine.getAudioFileFormatManager().readFormatManager;
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (render.renderFile));

    if (reader == nullptr)
    {
        discardPreroll (render);
        return "could not read the rendered audio back";
    }

    const auto total = reader->lengthInSamples;
    const auto keep = juce::jmax ((juce::int64) 0, total - render.prerollSamples);
    const auto numChannels = (int) reader->numChannels;
    const auto sampleRate = reader->sampleRate;

    auto* format = engine.getAudioFileFormatManager().getFormatFromFileName (render.destination);

    if (format == nullptr)
        format = engine.getAudioFileFormatManager().getDefaultFormat();

    render.destination.deleteFile();
    std::unique_ptr<juce::OutputStream> out (render.destination.createOutputStream());

    if (out == nullptr)
    {
        discardPreroll (render);
        return "could not write " + render.destination.getFileName();
    }

    std::unique_ptr<juce::AudioFormatWriter> writer (
        format->createWriterFor (out.get(), sampleRate, (unsigned int) numChannels, bitDepth, {}, 0));

    if (writer == nullptr)
    {
        discardPreroll (render);
        render.destination.deleteFile();
        return "could not write " + render.destination.getFileName();
    }

    out.release();   // the writer owns it now

    // Through float, which is lossless both ways for the integer depths the
    // renderer writes: an n-bit sample read as float and written back as n
    // bits is the same sample.
    constexpr int chunk = 65536;
    juce::AudioBuffer<float> buffer (numChannels, chunk);
    bool ok = true;

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
        ok = reader->read (&buffer, 0, n, render.prerollSamples + done, true, true)
              && writer->writeFromAudioSampleBuffer (buffer, 0, n);
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

    return {};
}

void discardPreroll (const PrerollRender& render)
{
    render.renderFile.deleteFile();
}

} // namespace carve::sync
