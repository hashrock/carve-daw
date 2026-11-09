#include "UtauPlugin.h"

#include "model/SongModel.h"

namespace carve::plugins
{

const char* UtauPlugin::xmlTypeName = "carveUtau";

namespace
{
    // A note that is no longer in the song keeps its audio this long, so an
    // undo straight after an edit plays at once instead of synthesising again.
    constexpr double keepUnwantedMs = 20000.0;

    // A snippet let go of is freed only after this long with nothing reading
    // it: the audio thread looks a snippet up and then marks it as playing,
    // and this is what makes that window harmless.
    constexpr double retireGraceMs = 5000.0;

    // Where a phrase ends with nothing after it, the voice fades out over the
    // last of the note rather than stopping on a sample.
    constexpr double releaseMs = 30.0;

    // The fade-in a note gets when nothing before it overlaps it, against a
    // click from a recording that does not start on silence.
    constexpr double attackMs = 5.0;

    constexpr juce::uint8 triggerHeader[] = { 0x7d, 0x43, 0x55 };   // non-commercial id, "CU"

    double nowMs()  { return juce::Time::getMillisecondCounterHiRes(); }

    // UTAU's own rule for a note that follows a short one: the preutterance
    // and overlap together may take at most half the note before, and are
    // shrunk in proportion to fit.
    void scaleToPrevious (double& preutterMs, double& overlapMs, double previousLengthMs)
    {
        const auto taken = preutterMs - overlapMs;
        const auto allowed = previousLengthMs * 0.5;

        if (taken > allowed && taken > 0.0)
        {
            const auto factor = juce::jmax (0.0, allowed / taken);
            preutterMs *= factor;
            overlapMs *= factor;
        }
    }

    juce::String lyricOrDefault (const juce::String& lyric)
    {
        return lyric.trim().isNotEmpty() ? lyric.trim() : model::Note::getDefaultLyric();
    }
}

UtauPlugin::UtauPlugin (te::PluginCreationInfo info)
    : te::Plugin (info),
      table (new std::atomic<Snippet*>[(size_t) maxSnippets])
{
    for (int i = 0; i < maxSnippets; ++i)
        table[(size_t) i].store (nullptr, std::memory_order_relaxed);

    startTimer (2000);
}

UtauPlugin::~UtauPlugin()
{
    notifyListenersOfDeletion();
    stopTimer();
}

//==============================================================================
// The worker threads. One pool for every singer: a song with three vocal parts
// should not start nine threads, and a synthesis is the same work whoever
// asked for it.
namespace
{
    juce::ThreadPool& renderPool()
    {
        static juce::ThreadPool pool (juce::ThreadPoolOptions{}
                                          .withThreadName ("Carve UTAU")
                                          .withNumberOfThreads (juce::jlimit (1, 4, juce::SystemStats::getNumCpus() - 1)));
        return pool;
    }
}

bool UtauPlugin::waitForRenders (int timeoutMs) const
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

    while (renderState->pending.load() > 0)
    {
        if (juce::Time::getMillisecondCounter() > deadline)
            return false;

        juce::Thread::sleep (5);
    }

    return true;
}

juce::String UtauPlugin::getLastRenderError() const
{
    const juce::ScopedLock sl (renderState->errorLock);
    return renderState->lastError;
}

//==============================================================================
void UtauPlugin::setVoicebank (const juce::File& folder)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (folder == juce::File())
    {
        if (bank != nullptr || loadError.isNotEmpty())
        {
            bank.reset();
            loadError = {};
            dropAllSnippets();
        }

        return;
    }

    juce::String error;
    auto loaded = voice::Voicebank::loadShared (folder, error);

    if (loaded == bank && error == loadError)
        return;

    bank = loaded;
    loadError = error;
    dropAllSnippets();

    // The live-note recording, read now rather than on the first key press.
    std::shared_ptr<voice::RawSample> sample;

    if (bank != nullptr)
        if (auto* entry = bank->getPreviewEntry())
        {
            juce::String sampleError;

            if (auto raw = voice::readRawSample (*entry, sampleError))
                sample = std::make_shared<voice::RawSample> (std::move (*raw));
        }

    {
        const juce::ScopedLock sl (liveLock);
        std::swap (liveSample, sample);
    }

    if (sample != nullptr)
        retiredLiveSamples.emplace_back (std::move (sample), nowMs());
}

void UtauPlugin::dropAllSnippets()
{
    for (auto& [id, slot] : slots)
        retire (id);

    slots.clear();
    idsByKey.clear();
}

void UtauPlugin::retire (int id)
{
    const auto it = slots.find (id);

    if (it == slots.end())
        return;

    table[(size_t) id].store (nullptr);
    retired.push_back ({ it->second.snippet, nowMs() });
}

void UtauPlugin::beginPreparing()
{
    ++generation;
}

UtauPlugin::Prepared UtauPlugin::prepare (const NoteContext& note)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (bank == nullptr)
        return {};

    const auto lyric = lyricOrDefault (note.lyric);
    const auto previousLyric = note.hasPrevious ? lyricOrDefault (note.previousLyric) : juce::String();
    const auto* entry = bank->resolve (lyric, previousLyric, note.pitch);

    if (entry == nullptr)
        return {};

    voice::NoteRender request;
    request.entry = *entry;

    auto preutterMs = entry->preutterMs;
    auto overlapMs = entry->overlapMs;

    if (note.hasPrevious)
        scaleToPrevious (preutterMs, overlapMs, note.previousLengthSec * 1000.0);

    const auto lengthMs = note.lengthSec * 1000.0;

    // Where this note's sound ends, from its start: under the next note's
    // head when one follows straight on -- that note's own sound starts its
    // preutterance before it, and the two overlap by its overlap -- or at its
    // own end, fading.
    auto endMs = lengthMs;
    auto fadeOutMs = juce::jmin (releaseMs, lengthMs * 0.5);

    if (note.hasNext)
    {
        if (const auto* next = bank->resolve (lyricOrDefault (note.nextLyric), lyric, note.nextPitch))
        {
            auto nextPreutterMs = next->preutterMs;
            auto nextOverlapMs = next->overlapMs;
            scaleToPrevious (nextPreutterMs, nextOverlapMs, lengthMs);

            endMs = note.nextStartSec * 1000.0 - nextPreutterMs + nextOverlapMs;
            fadeOutMs = juce::jmax (0.0, nextOverlapMs);
        }
    }

    endMs = juce::jmax (endMs, 10.0);

    request.preutterMs = preutterMs;
    request.lengthMs = preutterMs + endMs;
    request.targetHz = voice::hzForMidiNote (note.pitch);

    if (note.hasPrevious)
        request.fromHz = voice::hzForMidiNote (note.previousPitch);

    request.gain = (float) note.velocity / 100.0f;
    request.fadeInMs = note.hasPrevious ? juce::jmax (attackMs, overlapMs) : attackMs;
    request.fadeOutMs = juce::jmin (fadeOutMs, request.lengthMs);

    request.gender = style.gender;
    request.breath = style.breath;
    request.brightness = style.brightness;
    request.modulation = style.modulation;

    // Everything the audio depends on, so the same note in the same place
    // finds the snippet it already has.
    const auto key = entry->file.getFullPathName() + "|" + entry->alias
                     + "|" + juce::String (preutterMs, 2) + "|" + juce::String (request.lengthMs, 2)
                     + "|" + juce::String (note.pitch)
                     + "|" + (request.fromHz ? juce::String (note.previousPitch) : juce::String ("-"))
                     + "|" + juce::String (note.velocity)
                     + "|" + juce::String (request.fadeInMs, 2) + "|" + juce::String (request.fadeOutMs, 2)
                     + "|" + juce::String (style.gender, 1) + "|" + juce::String (style.breath, 1)
                     + "|" + juce::String (style.brightness, 1) + "|" + juce::String (style.modulation, 1);

    if (const auto it = idsByKey.find (key); it != idsByKey.end())
    {
        auto& slot = slots[it->second];
        slot.generation = generation;
        slot.unwantedSinceMs = 0.0;
        slot.snippet->wanted.store (true);

        // Skipped while it was out of the song, and back in it now.
        if (slot.snippet->skipped.exchange (false))
            submit (slot.snippet, slot.request);

        return { it->second, preutterMs / 1000.0 };
    }

    // The next id whose slot is free. One that is still in use is skipped,
    // which can only happen after 65536 notes have come and gone.
    int id = -1;

    for (int tries = 0; tries < maxSnippets; ++tries)
    {
        const auto candidate = nextId;
        nextId = (nextId + 1) % maxSnippets;

        if (slots.find (candidate) == slots.end() && table[(size_t) candidate].load() == nullptr)
        {
            id = candidate;
            break;
        }
    }

    if (id < 0)
        return {};

    auto snippet = std::make_shared<Snippet>();
    slots[id] = { key, snippet, generation, 0.0, request };
    idsByKey[key] = id;
    table[(size_t) id].store (snippet.get());

    submit (snippet, request);
    return { id, preutterMs / 1000.0 };
}

void UtauPlugin::submit (std::shared_ptr<Snippet> snippet, const voice::NoteRender& request)
{
    auto jobState = renderState;
    ++jobState->pending;

    renderPool().addJob ([snippet, request, jobState]
    {
        if (! snippet->wanted.load())
        {
            // Marked skipped before looking again, so a prepare() that wants
            // it back in between sees the mark and resubmits -- or, if it
            // came before the mark, this sees it wanted and takes the mark
            // back itself. Exactly one of the two goes on to synthesise.
            snippet->skipped.store (true);

            if (! snippet->wanted.load() || ! snippet->skipped.exchange (false))
            {
                --jobState->pending;
                return;
            }
        }

        juce::String error;

        if (auto rendered = voice::renderNote (request, error))
        {
            snippet->audio = std::move (rendered->audio);
            snippet->sampleRate = rendered->sampleRate;
            snippet->ready.store (true, std::memory_order_release);
        }
        else
        {
            ++jobState->failed;
            const juce::ScopedLock sl (jobState->errorLock);
            jobState->lastError = error;
        }

        --jobState->pending;
    });
}

void UtauPlugin::endPreparing()
{
    const auto now = nowMs();

    for (auto& [id, slot] : slots)
        if (slot.generation != generation && juce::exactlyEqual (slot.unwantedSinceMs, 0.0))
        {
            slot.unwantedSinceMs = now;
            slot.snippet->wanted.store (false);
        }
}

void UtauPlugin::timerCallback()
{
    const auto now = nowMs();

    for (auto it = slots.begin(); it != slots.end();)
    {
        if (it->second.unwantedSinceMs > 0.0 && now - it->second.unwantedSinceMs > keepUnwantedMs)
        {
            retire (it->first);
            idsByKey.erase (it->second.key);
            it = slots.erase (it);
        }
        else
        {
            ++it;
        }
    }

    retiredLiveSamples.erase (std::remove_if (retiredLiveSamples.begin(), retiredLiveSamples.end(), [now] (const auto& r)
    {
        return now - r.second > 60000.0;
    }), retiredLiveSamples.end());

    retired.erase (std::remove_if (retired.begin(), retired.end(), [now] (const Retired& r)
    {
        return now - r.sinceMs > retireGraceMs && r.snippet->playing.load() == 0;
    }), retired.end());
}

//==============================================================================
juce::MidiMessage UtauPlugin::createTrigger (int id, double skipSec)
{
    const auto skip = juce::jlimit (0, (1 << 21) - 1, (int) std::lround (skipSec * 10000.0));   // 0.1 ms units

    const juce::uint8 data[] = { triggerHeader[0], triggerHeader[1], triggerHeader[2],
                                 (juce::uint8) ((id >> 14) & 0x7f), (juce::uint8) ((id >> 7) & 0x7f), (juce::uint8) (id & 0x7f),
                                 (juce::uint8) ((skip >> 14) & 0x7f), (juce::uint8) ((skip >> 7) & 0x7f), (juce::uint8) (skip & 0x7f) };

    return juce::MidiMessage::createSysExMessage (data, (int) sizeof (data));
}

bool UtauPlugin::parseTrigger (const juce::MidiMessage& m, int& id, double& skipSec)
{
    if (! m.isSysEx() || m.getSysExDataSize() != 9)
        return false;

    const auto* d = m.getSysExData();

    if (d[0] != triggerHeader[0] || d[1] != triggerHeader[1] || d[2] != triggerHeader[2])
        return false;

    id = (d[3] << 14) | (d[4] << 7) | d[5];
    skipSec = ((d[6] << 14) | (d[7] << 7) | d[8]) / 10000.0;
    return true;
}

//==============================================================================
void UtauPlugin::initialise (const te::PluginInitialisationInfo&)
{
    // Plugin::sampleRate is the base class's, already set from the info.
}

void UtauPlugin::deinitialise()
{
}

void UtauPlugin::reset()
{
    // The voices are the audio thread's; a reset reaches them through
    // applyToBuffer's all-notes-off instead.
}

void UtauPlugin::finish (Voice& voice)
{
    if (voice.snippet != nullptr)
        voice.snippet->playing.fetch_sub (1);

    voice = {};
}

void UtauPlugin::stopAll (int fadeSamples)
{
    for (auto& voice : voices)
        if (voice.active && voice.releaseLeft < 0)
        {
            voice.releaseLength = juce::jmax (1, fadeSamples);
            voice.releaseLeft = voice.releaseLength;
        }
}

void UtauPlugin::startSnippet (int id, double skipSec)
{
    if (id < 0 || id >= maxSnippets)
        return;

    auto* snippet = table[(size_t) id].load();

    if (snippet == nullptr || ! snippet->ready.load (std::memory_order_acquire))
        return;   // not synthesised yet: this time round, the note is silent

    for (auto& voice : voices)
    {
        if (voice.active)
            continue;

        snippet->playing.fetch_add (1);
        voice.active = true;
        voice.snippet = snippet;
        voice.step = snippet->sampleRate / sampleRate;
        voice.position = skipSec * snippet->sampleRate;
        voice.gain = 1.0f;
        return;
    }
}

void UtauPlugin::startLiveNote (int note, float velocity)
{
    const juce::ScopedTryLock sl (liveLock);

    if (! sl.isLocked() || liveSample == nullptr || liveSample->hz <= 0.0)
        return;

    for (auto& voice : voices)
    {
        if (voice.active)
            continue;

        voice.active = true;
        voice.raw = &liveSample->audio;
        voice.note = note;
        voice.step = voice::hzForMidiNote (note) / liveSample->hz * liveSample->sampleRate / sampleRate;
        voice.position = 0.0;
        voice.gain = velocity;
        return;
    }
}

void UtauPlugin::releaseLiveNote (int note)
{
    const auto fade = juce::jmax (1, (int) (releaseMs * 0.001 * sampleRate));

    for (auto& voice : voices)
        if (voice.active && voice.raw != nullptr && voice.note == note && voice.releaseLeft < 0)
        {
            voice.releaseLength = fade;
            voice.releaseLeft = fade;
        }
}

void UtauPlugin::renderVoices (juce::AudioBuffer<float>& buffer, int start, int numSamples)
{
    auto* left = buffer.getWritePointer (0, start);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1, start) : nullptr;

    for (auto& voice : voices)
    {
        if (! voice.active)
            continue;

        const auto& source = voice.snippet != nullptr ? voice.snippet->audio : *voice.raw;
        const auto* data = source.getReadPointer (0);
        const auto length = source.getNumSamples();

        for (int i = 0; i < numSamples; ++i)
        {
            const auto index = (int) voice.position;

            if (index + 1 >= length)
            {
                finish (voice);
                break;
            }

            const auto frac = (float) (voice.position - index);
            auto sample = (data[index] + (data[index + 1] - data[index]) * frac) * voice.gain;

            if (voice.releaseLeft >= 0)
            {
                sample *= (float) voice.releaseLeft / (float) voice.releaseLength;

                if (--voice.releaseLeft <= 0)
                {
                    finish (voice);
                    break;
                }
            }

            left[i] += sample;

            if (right != nullptr)
                right[i] += sample;

            voice.position += voice.step;
        }
    }
}

void UtauPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    auto& buffer = *fc.destBuffer;

    if (buffer.getNumChannels() == 0)
        return;

    buffer.clear (fc.bufferStartSample, fc.bufferNumSamples);

    if (fc.bufferForMidiMessages != nullptr && fc.bufferForMidiMessages->isAllNotesOff)
        stopAll ((int) (0.005 * sampleRate));

    int pos = 0;

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (const auto& m : *fc.bufferForMidiMessages)
        {
            const int at = juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * sampleRate));

            if (at > pos)
            {
                renderVoices (buffer, fc.bufferStartSample + pos, at - pos);
                pos = at;
            }

            int id = -1;
            double skip = 0.0;

            if (parseTrigger (m, id, skip))
                startSnippet (id, skip);
            else if (m.isNoteOn())
                startLiveNote (m.getNoteNumber(), m.getFloatVelocity());
            else if (m.isNoteOff())
                releaseLiveNote (m.getNoteNumber());
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                stopAll ((int) (0.005 * sampleRate));
        }
    }

    if (pos < fc.bufferNumSamples)
        renderVoices (buffer, fc.bufferStartSample + pos, fc.bufferNumSamples - pos);

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);
}

} // namespace carve::plugins
