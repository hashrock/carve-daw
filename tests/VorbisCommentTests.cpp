// Vorbis comments on a finished Ogg file (src/sync/VorbisComments.h): the
// game loop's LOOPSTART / LOOPLENGTH go in after JUCE has written the file,
// and what matters is that they are there, that everything else survives,
// and that the audio still decodes to the same samples.

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include "sync/VorbisComments.h"

namespace
{

using carve::sync::readVorbisComments;
using carve::sync::setVorbisComments;

// A second of two sines, as an Ogg Vorbis file JUCE wrote.
struct TempOgg
{
    explicit TempOgg (const juce::StringPairArray& metadata = {})
    {
        juce::AudioBuffer<float> buffer (2, 44100);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            buffer.setSample (0, i, 0.5f * std::sin ((float) i * 0.05f));
            buffer.setSample (1, i, 0.5f * std::sin ((float) i * 0.08f));
        }

        juce::OggVorbisAudioFormat format;
        std::unique_ptr<juce::OutputStream> out (file.getFile().createOutputStream());
        REQUIRE (out != nullptr);

        std::unique_ptr<juce::AudioFormatWriter> writer (
            format.createWriterFor (out.get(), 44100.0, 2, 16, metadata, 6));
        REQUIRE (writer != nullptr);
        out.release();

        REQUIRE (writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples()));
    }

    juce::AudioBuffer<float> decode() const
    {
        juce::OggVorbisAudioFormat format;
        std::unique_ptr<juce::AudioFormatReader> reader (
            format.createReaderFor (file.getFile().createInputStream().release(), true));
        REQUIRE (reader != nullptr);

        juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
        REQUIRE (reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true));
        return buffer;
    }

    juce::TemporaryFile file { ".ogg" };
};

bool sameSamples (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return false;

    for (int channel = 0; channel < a.getNumChannels(); ++channel)
        if (std::memcmp (a.getReadPointer (channel), b.getReadPointer (channel),
                         sizeof (float) * (size_t) a.getNumSamples()) != 0)
            return false;

    return true;
}

juce::StringPairArray loopPoints (const juce::String& start, const juce::String& length)
{
    juce::StringPairArray comments;
    comments.set ("LOOPSTART", start);
    comments.set ("LOOPLENGTH", length);
    return comments;
}

} // namespace

TEST_CASE ("loop points are written to an Ogg file and read back", "[vorbis]")
{
    TempOgg ogg;
    const auto before = ogg.decode();

    REQUIRE (setVorbisComments (ogg.file.getFile(), loopPoints ("12345", "67890")).isEmpty());

    const auto comments = readVorbisComments (ogg.file.getFile());
    CHECK (comments["LOOPSTART"] == "12345");
    CHECK (comments["LOOPLENGTH"] == "67890");

    // The audio pages were copied, not re-encoded.
    CHECK (sameSamples (before, ogg.decode()));
}

TEST_CASE ("comments the writer put there survive, and a name is replaced not repeated", "[vorbis]")
{
    juce::StringPairArray metadata;
    metadata.set (juce::OggVorbisAudioFormat::id3title, "A Title");
    TempOgg ogg (metadata);

    REQUIRE (setVorbisComments (ogg.file.getFile(), loopPoints ("1", "2")).isEmpty());

    juce::StringPairArray lower;
    lower.set ("loopstart", "3");
    REQUIRE (setVorbisComments (ogg.file.getFile(), lower).isEmpty());

    const auto comments = readVorbisComments (ogg.file.getFile());
    CHECK (comments["TITLE"] == "A Title");
    CHECK (comments["LOOPSTART"] == "3");
    CHECK (comments["LOOPLENGTH"] == "2");
    CHECK (comments.size() == 3);
}

TEST_CASE ("a comment header that grows by pages leaves a file that still decodes", "[vorbis]")
{
    TempOgg ogg;
    const auto before = ogg.decode();

    // A page holds at most 255 segments of 255 bytes, about 65 kB: this
    // pushes the header over several, so every audio page is renumbered.
    juce::StringPairArray big;
    big.set ("NOTES", juce::String::repeatedString ("x", 200000));
    REQUIRE (setVorbisComments (ogg.file.getFile(), big).isEmpty());

    CHECK (readVorbisComments (ogg.file.getFile())["NOTES"].length() == 200000);
    CHECK (sameSamples (before, ogg.decode()));

    // ...and back down again.
    big.set ("NOTES", "short");
    REQUIRE (setVorbisComments (ogg.file.getFile(), big).isEmpty());
    CHECK (readVorbisComments (ogg.file.getFile())["NOTES"] == "short");
    CHECK (sameSamples (before, ogg.decode()));
}

TEST_CASE ("a file that is not Ogg Vorbis is refused and left alone", "[vorbis]")
{
    juce::TemporaryFile file (".ogg");
    REQUIRE (file.getFile().replaceWithText ("RIFF, not Ogg"));

    CHECK (setVorbisComments (file.getFile(), loopPoints ("1", "2")).isNotEmpty());
    CHECK (file.getFile().loadFileAsString() == "RIFF, not Ogg");
    CHECK (readVorbisComments (file.getFile()).size() == 0);
}
