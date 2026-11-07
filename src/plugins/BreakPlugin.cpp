#include "BreakPlugin.h"

#include <cmath>
#include <map>
#include <mutex>

namespace carve::plugins
{

const char* BreakPlugin::xmlTypeName = "carveBreak";

namespace
{
    // Long enough that a cut mid-wave doesn't click, short enough that a hit
    // keeps its snap.
    constexpr double attackSeconds = 0.0005;
    constexpr double releaseSeconds = 0.003;

    // The file's loop, read now or shared from whoever read it already.
    std::shared_ptr<const BreakPlugin::Audio> loadAudio (const juce::File& file, juce::String& error)
    {
        static std::mutex mutex;
        static std::map<juce::String, std::weak_ptr<const BreakPlugin::Audio>> cache;
        const std::lock_guard<std::mutex> lock (mutex);

        const auto key = file.getFullPathName();
        const auto stamp = file.getLastModificationTime();

        for (auto it = cache.begin(); it != cache.end();)
            it = it->second.expired() ? cache.erase (it) : std::next (it);

        if (auto it = cache.find (key); it != cache.end())
            if (auto existing = it->second.lock(); existing != nullptr && existing->stamp == stamp)
                return existing;

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        {
            error = "Not an audio file this can read: " + file.getFileName();
            return {};
        }

        // A breakbeat is seconds long; anything past ten minutes is not one,
        // and reading it whole would be a lot of memory for a mistake.
        if (reader->lengthInSamples > (juce::int64) (reader->sampleRate * 600.0))
        {
            error = "Too long for a loop: " + file.getFileName();
            return {};
        }

        auto loaded = std::make_shared<BreakPlugin::Audio>();
        loaded->file = file;
        loaded->stamp = stamp;
        loaded->sampleRate = reader->sampleRate;

        const int numFrames = (int) reader->lengthInSamples;
        loaded->buffer.setSize (2, numFrames);
        reader->read (&loaded->buffer, 0, numFrames, 0, true, true);

        // A mono file reads into the left channel only.
        if (reader->numChannels == 1)
            loaded->buffer.copyFrom (1, 0, loaded->buffer, 0, 0, numFrames);

        cache[key] = loaded;
        return loaded;
    }
}

BreakPlugin::BreakPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
}

BreakPlugin::~BreakPlugin()
{
    notifyListenersOfDeletion();

    std::unique_ptr<Program> old;

    const juce::ScopedLock sl (programLock);
    std::swap (old, program);
}

double BreakPlugin::getPlaybackRate() const
{
    const juce::ScopedLock sl (programLock);
    return program != nullptr ? program->rate : 1.0;
}

void BreakPlugin::setSource (const juce::File& file, const std::vector<double>& sliceStarts,
                             double repitchLoopBeats, double songBpm)
{
    JUCE_ASSERT_MESSAGE_THREAD

    std::shared_ptr<const Audio> newAudio;

    if (file == juce::File())
        loadError = {};
    else if (! file.existsAsFile())
        loadError = "File not found: " + file.getFileName();
    else if (audio != nullptr && audio->file == file && audio->stamp == file.getLastModificationTime())
        newAudio = audio, loadError = {};
    else
    {
        loadError = {};
        newAudio = loadAudio (file, loadError);
    }

    auto next = std::make_unique<Program>();
    next->audio = newAudio;
    next->rate = 1.0;

    // The loop lasts repitchLoopBeats of the song's beats: its own length
    // over what those beats last at the song's tempo.
    if (newAudio != nullptr && repitchLoopBeats > 0.0 && songBpm > 0.0)
        next->rate = juce::jlimit (0.125, 8.0, newAudio->getLengthSeconds() / (repitchLoopBeats * 60.0 / songBpm));

    if (newAudio != nullptr)
    {
        const double length = newAudio->buffer.getNumSamples();

        for (auto start : sliceStarts)
            next->sliceStartFrames.push_back (juce::jlimit (0.0, length, start * newAudio->sampleRate));
    }

    // Nothing changed, nothing to swap: a sync of an unrelated edit must not
    // cost a lock the audio thread may want.
    {
        const juce::ScopedLock sl (programLock);

        if (program != nullptr && program->audio == next->audio && std::abs (program->rate - next->rate) < 1.0e-12
             && program->sliceStartFrames == next->sliceStartFrames)
            return;
    }

    std::unique_ptr<Program> old;

    {
        const juce::ScopedLock sl (programLock);

        // A different loop: what was sounding was a slice of the old one.
        if (program == nullptr || program->audio != next->audio)
            for (auto& voice : voices)
                voice.active = false;

        old = std::exchange (program, std::move (next));
    }

    audio = newAudio;
}

void BreakPlugin::initialise (const te::PluginInitialisationInfo&)
{
    // Plugin::sampleRate, which the base class has set already.
    attackStep = (float) (1.0 / juce::jmax (1.0, attackSeconds * sampleRate));
    releaseStep = (float) (1.0 / juce::jmax (1.0, releaseSeconds * sampleRate));

    const juce::ScopedLock sl (programLock);

    for (auto& voice : voices)
        voice.active = false;
}

void BreakPlugin::deinitialise()
{
}

void BreakPlugin::reset()
{
    const juce::ScopedLock sl (programLock);

    for (auto& voice : voices)
        voice.active = false;

    playingSlice.store (-1, std::memory_order_relaxed);
}

void BreakPlugin::startSlice (int note, float velocity)
{
    const int slice = note - firstNote;
    const auto& starts = program->sliceStartFrames;

    if (slice < 0 || slice >= (int) starts.size() || program->audio == nullptr)
        return;

    // Choke whatever is sounding.
    releaseAll();

    // A free voice, or failing that the one furthest into its fade.
    Voice* chosen = &voices[0];

    for (auto& voice : voices)
    {
        if (! voice.active)
        {
            chosen = &voice;
            break;
        }

        if (voice.envelope < chosen->envelope)
            chosen = &voice;
    }

    auto& voice = *chosen;
    voice.active = true;
    voice.note = note;
    voice.slice = slice;
    voice.position = starts[(size_t) slice];
    voice.end = slice + 1 < (int) starts.size() ? starts[(size_t) slice + 1]
                                                : (double) program->audio->buffer.getNumSamples();
    voice.gain = velocity;
    voice.envelope = 0.0f;
    voice.envelopeStep = attackStep;
}

void BreakPlugin::releaseNote (int note)
{
    for (auto& voice : voices)
        if (voice.active && voice.note == note && voice.envelopeStep > 0.0f)
            voice.envelopeStep = -releaseStep;
}

void BreakPlugin::releaseAll()
{
    for (auto& voice : voices)
        if (voice.active && voice.envelopeStep > 0.0f)
            voice.envelopeStep = -releaseStep;
}

void BreakPlugin::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
        startSlice (m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff())
        releaseNote (m.getNoteNumber());
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        releaseAll();
}

void BreakPlugin::renderSegment (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    const auto& source = program->audio->buffer;
    const int sourceFrames = source.getNumSamples();
    const float* inL = source.getReadPointer (0);
    const float* inR = source.getReadPointer (1);

    float* outL = buffer.getWritePointer (0, startSample);
    float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1, startSample) : nullptr;

    const double step = program->audio->sampleRate / sampleRate * program->rate;

    // How many output samples the fade at a slice's end takes, in file frames:
    // the slice starts fading that far before the next cut, so it reaches
    // silence on it rather than being cut off there.
    const double endFadeFrames = step / juce::jmax (1.0e-6, (double) releaseStep);

    for (auto& voice : voices)
    {
        if (! voice.active)
            continue;

        for (int i = 0; i < numSamples; ++i)
        {
            if (voice.envelopeStep > 0.0f && voice.position >= voice.end - endFadeFrames)
                voice.envelopeStep = -releaseStep;

            voice.envelope += voice.envelopeStep;

            if (voice.envelope >= 1.0f)
            {
                voice.envelope = 1.0f;
                voice.envelopeStep = 0.0f;
            }

            if (voice.envelope <= 0.0f || voice.position >= voice.end || voice.position >= sourceFrames - 1)
            {
                voice.active = false;
                break;
            }

            const int index = (int) voice.position;
            const auto fraction = (float) (voice.position - index);
            const float gain = voice.envelope * voice.gain;

            const float left = inL[index] + (inL[index + 1] - inL[index]) * fraction;
            const float right = inR[index] + (inR[index + 1] - inR[index]) * fraction;

            if (outR != nullptr)
            {
                outL[i] += left * gain;
                outR[i] += right * gain;
            }
            else
            {
                outL[i] += 0.5f * (left + right) * gain;
            }

            voice.position += step;
        }
    }
}

void BreakPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    auto& buffer = *fc.destBuffer;

    if (buffer.getNumChannels() == 0)
        return;

    buffer.clear (fc.bufferStartSample, fc.bufferNumSamples);

    // A loop being swapped in is a block of silence, never a wait.
    const juce::ScopedTryLock sl (programLock);

    if (! sl.isLocked() || program == nullptr || program->audio == nullptr)
        return;

    if (fc.bufferForMidiMessages != nullptr && fc.bufferForMidiMessages->isAllNotesOff)
        for (auto& voice : voices)
            voice.active = false;

    int pos = 0;

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (const auto& m : *fc.bufferForMidiMessages)
        {
            const int at = juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * sampleRate));

            if (at > pos)
            {
                renderSegment (buffer, fc.bufferStartSample + pos, at - pos);
                pos = at;
            }

            handleMidi (m);
        }
    }

    if (pos < fc.bufferNumSamples)
        renderSegment (buffer, fc.bufferStartSample + pos, fc.bufferNumSamples - pos);

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);

    // The sounding slice for the editor: the one fading in or held, not the
    // ones choked.
    int slice = -1;
    double seconds = 0.0;

    for (const auto& voice : voices)
        if (voice.active && voice.envelopeStep >= 0.0f)
        {
            slice = voice.slice;
            seconds = voice.position / program->audio->sampleRate;
        }

    playingSlice.store (slice, std::memory_order_relaxed);
    playingSeconds.store (seconds, std::memory_order_relaxed);
}

} // namespace carve::plugins
