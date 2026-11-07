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

    // How far apart a trigger and its note-on may arrive. They are written at
    // the same beat, but the engine does not promise the same sample -- a
    // block boundary can fall between them -- so they are paired within a
    // short window either way. Shorter than any roll's restarts, so a
    // trigger can never be taken by the next note of the same slice.
    constexpr double pairingWindowSeconds = 0.02;

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

    for (auto& trigger : pending)
        trigger.set = false;

    anyPending = false;
    playingSlice.store (-1, std::memory_order_relaxed);
}

void BreakPlugin::applyMode (Voice& voice, const PendingTrigger& trigger)
{
    const auto mode = trigger.mode;
    voice.mode = mode;
    voice.duration = trigger.seconds > 0.0 ? trigger.seconds * sampleRate : 0.0;
    voice.buzzFrom = trigger.buzzFrom;
    voice.buzzTo = trigger.buzzTo;
    voice.cycle = 0.0;

    // Backwards starts from the far end of the slice; everything else from
    // its cut.
    voice.position = mode == Mode::reverse ? juce::jmax (voice.start, voice.end - 1.0) : voice.start;

    // A tape stop coasts on past the next cut: slowing to a halt over a
    // whole note takes more tape than one slice holds, and stopping dead at
    // the cut would be a stop of a different kind.
    if (mode == Mode::tapeStop && program != nullptr && program->audio != nullptr)
        voice.end = (double) program->audio->buffer.getNumSamples();
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
    voice.start = starts[(size_t) slice];
    voice.end = slice + 1 < (int) starts.size() ? starts[(size_t) slice + 1]
                                                : (double) program->audio->buffer.getNumSamples();
    voice.elapsed = 0.0;
    voice.gain = velocity;
    voice.envelope = 0.0f;
    voice.envelopeStep = attackStep;

    auto& trigger = pending[(size_t) note];
    applyMode (voice, trigger.set ? trigger : PendingTrigger {});
    trigger.set = false;
}

void BreakPlugin::handleTrigger (const juce::MidiMessage& m)
{
    const auto* data = m.getSysExData();

    const auto size = m.getSysExDataSize();

    if ((size != 8 && size != 12) || data[0] != sysexId[0] || data[1] != sysexId[1] || data[2] != sysexId[2])
        return;

    auto threeBytes = [data] (int at)
    {
        return ((data[at] & 0x7f) << 14) | ((data[at + 1] & 0x7f) << 7) | (data[at + 2] & 0x7f);
    };

    PendingTrigger trigger;
    trigger.set = true;
    trigger.mode = (Mode) juce::jlimit (0, 6, (int) data[3]);
    trigger.seconds = threeBytes (5) / 1000.0;

    const int note = data[4] & 0x7f;

    if (trigger.mode == Mode::buzz)
    {
        if (size != 12)
            return;

        trigger.buzzTo = juce::MidiMessage::getMidiNoteInHertz (data[8] & 0x7f);
        trigger.buzzFrom = threeBytes (9) / 10.0;
    }

    // After its note-on: the voice it is for has only just started.
    for (auto& voice : voices)
        if (voice.active && voice.note == note && voice.elapsed < pairingWindowSeconds * sampleRate
             && voice.isHeld())
        {
            applyMode (voice, trigger);
            return;
        }

    trigger.age = 0.0;
    pending[(size_t) note] = trigger;
    anyPending = true;
}

void BreakPlugin::releaseNote (int note)
{
    for (auto& voice : voices)
        if (voice.active && voice.note == note && voice.isHeld())
            voice.envelopeStep = -releaseStep;
}

void BreakPlugin::releaseAll()
{
    for (auto& voice : voices)
        if (voice.active && voice.isHeld())
            voice.envelopeStep = -releaseStep;
}

void BreakPlugin::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
        startSlice (m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff())
        releaseNote (m.getNoteNumber());
    else if (m.isSysEx())
        handleTrigger (m);
    else if (m.isAllNotesOff() || m.isAllSoundOff())
        releaseAll();
}

namespace
{
    // How fast a voice moves through its slice, as a multiple of normal
    // speed, `progress` of the way through its note (0..1).
    double speedFor (BreakPlugin::Mode mode, double progress)
    {
        switch (mode)
        {
            case BreakPlugin::Mode::reverse:
                return -1.0;

            case BreakPlugin::Mode::tapeStop:
                // The motor losing power: slowing all the way to nothing by
                // the note's end, faster at first, the way a platter coasts.
                return std::pow (juce::jmax (0.0, 1.0 - progress), 1.6);

            case BreakPlugin::Mode::scratch:
                // A baby scratch: the record pushed forward at its own speed
                // for the first half of the note, pulled straight back for
                // the second, ending where it started. Pitch stays as
                // recorded; only the direction changes. A roll of short
                // scratch notes is a run of strokes.
                return progress < 0.5 ? 1.0 : -1.0;

            case BreakPlugin::Mode::fadeIn:
            case BreakPlugin::Mode::fadeOut:
            case BreakPlugin::Mode::buzz:
            case BreakPlugin::Mode::forward:
                break;
        }

        return 1.0;
    }

    // How loud a voice is, `progress` of the way through its note: a fade in
    // rises from silence to full over the note, a fade out falls the other
    // way; everything else is at full throughout.
    float fadeGainFor (BreakPlugin::Mode mode, double progress)
    {
        if (mode == BreakPlugin::Mode::fadeIn)
            return (float) progress;

        if (mode == BreakPlugin::Mode::fadeOut)
            return (float) (1.0 - progress);

        return 1.0f;
    }

    // A tape stop or a scratch whose note length no trigger gave: what a
    // sixteenth at a breakbeat tempo lasts, give or take.
    constexpr double defaultEffectSeconds = 0.25;
}

void BreakPlugin::renderSegment (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    if (numSamples <= 0)
        return;

    // A trigger nothing has claimed for a moment was for a note that never
    // came; it must not wait for a later one.
    if (anyPending)
    {
        anyPending = false;

        for (auto& trigger : pending)
            if (trigger.set)
            {
                trigger.age += numSamples;
                trigger.set = trigger.age < pairingWindowSeconds * sampleRate;
                anyPending = anyPending || trigger.set;
            }
    }

    const auto& source = program->audio->buffer;
    const int sourceFrames = source.getNumSamples();
    const float* inL = source.getReadPointer (0);
    const float* inR = source.getReadPointer (1);

    float* outL = buffer.getWritePointer (0, startSample);
    float* outR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1, startSample) : nullptr;

    const double step = program->audio->sampleRate / sampleRate * program->rate;

    for (auto& voice : voices)
    {
        if (! voice.active)
            continue;

        const double duration = voice.duration > 0.0 ? voice.duration : defaultEffectSeconds * sampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            const double progress = juce::jmin (1.0, voice.elapsed / duration);
            const double speed = speedFor (voice.mode, progress);
            const double move = step * speed;

            // Fade out in time to reach silence at the slice's edge in the
            // direction of travel -- the next cut going forwards, this one
            // going backwards -- rather than being cut off there. A scratch
            // turns round inside its slice, so it fades only at its note's
            // end; a tape stop fades as it comes to rest.
            if (voice.isHeld())
            {
                const double fadeSamples = 1.0 / juce::jmax (1.0e-6, (double) releaseStep);
                bool fade = false;

                if (voice.mode == Mode::scratch || voice.mode == Mode::buzz)
                    fade = voice.elapsed >= duration - fadeSamples;
                else if (move > 0.0)
                    fade = voice.position + move * fadeSamples >= voice.end;
                else if (move < 0.0)
                    fade = voice.position + move * fadeSamples <= voice.start;

                if (voice.mode == Mode::tapeStop && progress >= 1.0 - 1.0e-3)
                    fade = true;

                if (fade)
                    voice.envelopeStep = -releaseStep;
            }

            voice.envelope += voice.envelopeStep;

            if (voice.envelope >= 1.0f)
            {
                voice.envelope = 1.0f;
                voice.envelopeStep = 0.0f;
            }

            const bool outside = voice.mode == Mode::scratch || voice.mode == Mode::buzz
                                    ? false
                                    : (voice.position >= voice.end || voice.position < voice.start);

            // A buzz is its slice's head over and over, `period` output
            // samples at a time, each restart faded in and out over a few
            // samples so the buzz is the slice's colour rather than a click
            // train. With a ramp the rate climbs from where it starts to
            // where it ends evenly in pitch, so a roll speeding up into a
            // note sounds like it is heading there.
            float grainGain = 1.0f;
            double period = 0.0;

            if (voice.mode == Mode::buzz)
            {
                const auto to = juce::jmax (1.0, voice.buzzTo);
                const auto from = voice.buzzFrom > 0.0 ? voice.buzzFrom : to;
                const auto rate = juce::jlimit (1.0, 4000.0, from * std::pow (to / from, progress));
                period = sampleRate / rate;

                const auto grainFade = juce::jmax (1.0, juce::jmin (32.0, period / 4.0));
                grainGain = (float) (juce::jmin (1.0, voice.cycle / grainFade)
                                       * juce::jmin (1.0, (period - voice.cycle) / grainFade));

                // A restart longer than the slice runs out of slice: the rest
                // of it is silence, not the next hit.
                if (voice.position >= voice.end - 1.0)
                    grainGain = 0.0f;
            }

            if (voice.envelope <= 0.0f || outside || voice.position >= sourceFrames - 1 || voice.position < 0.0)
            {
                voice.active = false;
                break;
            }

            const int index = (int) voice.position;
            const auto fraction = (float) (voice.position - index);
            const float gain = voice.envelope * voice.gain * fadeGainFor (voice.mode, progress) * grainGain;

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

            voice.position += move;
            voice.elapsed += 1.0;

            if (voice.mode == Mode::buzz)
            {
                voice.position = juce::jmin (voice.position, juce::jmax (voice.start, voice.end - 1.0));
                voice.cycle += 1.0;

                // Back to the head, keeping the fraction of a sample the
                // period overshot by: a restart rounded to whole samples
                // would wobble the pitch of any note whose period is not
                // a whole number of them.
                if (voice.cycle >= period)
                {
                    voice.cycle -= period;
                    voice.position = voice.start + voice.cycle * step;
                }
            }

            // A scratch stays on its slice: pushed past either edge, the
            // record just stops there until the hand turns it round.
            if (voice.mode == Mode::scratch)
                voice.position = juce::jlimit (voice.start, juce::jmax (voice.start, voice.end - 1.0), voice.position);
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
