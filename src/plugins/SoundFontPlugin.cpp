#include "SoundFontPlugin.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

// The one translation unit that compiles TinySoundFont's implementation.
#if defined (__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Wconversion"
 #pragma clang diagnostic ignored "-Wsign-conversion"
 #pragma clang diagnostic ignored "-Wshorten-64-to-32"
 #pragma clang diagnostic ignored "-Wimplicit-float-conversion"
 #pragma clang diagnostic ignored "-Wimplicit-int-conversion"
 #pragma clang diagnostic ignored "-Wfloat-conversion"
 #pragma clang diagnostic ignored "-Wcast-align"
 #pragma clang diagnostic ignored "-Wunused-parameter"
 #pragma clang diagnostic ignored "-Wmissing-prototypes"
 #pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
 #pragma clang diagnostic ignored "-Wold-style-cast"
 #pragma clang diagnostic ignored "-Wdouble-promotion"
 #pragma clang diagnostic ignored "-Wshadow"
#elif defined (__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wconversion"
 #pragma GCC diagnostic ignored "-Wsign-conversion"
 #pragma GCC diagnostic ignored "-Wfloat-conversion"
 #pragma GCC diagnostic ignored "-Wunused-parameter"
 #pragma GCC diagnostic ignored "-Wshadow"
#endif

#define TSF_IMPLEMENTATION
#include <tsf.h>

#if defined (__clang__)
 #pragma clang diagnostic pop
#elif defined (__GNUC__)
 #pragma GCC diagnostic pop
#endif

namespace carve::plugins
{

const char* SoundFontPlugin::xmlTypeName = "carveSoundFont";

namespace
{
    // Polyphony per generator. Fixed and allocated up front, because a voice
    // TinySoundFont has to make on a note-on is a realloc on the audio thread;
    // with a ceiling, a note past it steals the voice furthest into its
    // release instead. A held chord under a sustain pedal with a layered
    // preset (two regions a key) is the case this has to cover.
    constexpr int maxVoices = 128;

    // Unity. Fonts already leave room for chords in their own sample levels
    // (a GM font has to play sixteen channels at once), and the generator's
    // fader and the master limiter are where the rest of the headroom is.
    constexpr float outputGainDb = 0.0f;

    // The channel this player is. A generator is one instrument, so all of
    // its MIDI lands on one channel whatever channel it came in on.
    constexpr int channel = 0;

    // TinySoundFont's stream interface, over a JUCE stream, so a path with
    // characters fopen would choke on (or a Windows wide path) still opens.
    struct StreamAdapter
    {
        static int read (void* data, void* ptr, unsigned int size)
        {
            return (int) static_cast<juce::InputStream*> (data)->read (ptr, (size_t) size);
        }

        static int skip (void* data, unsigned int count)
        {
            auto* in = static_cast<juce::InputStream*> (data);
            const auto target = in->getPosition() + (juce::int64) count;
            return in->setPosition (target) && in->getPosition() == target ? 1 : 0;
        }
    };

    // Guards the cache and every tsf_copy / tsf_close. A font's sharing count
    // is a plain int inside TinySoundFont, so copying and closing must never
    // race -- in practice all of it happens on the message thread, and this
    // is what makes "in practice" not matter. Recursive because releasing a
    // Font closes its master, which takes the lock, and a Font can be
    // released while the cache is being swept with the lock held.
    std::recursive_mutex& fontMutex()
    {
        static std::recursive_mutex m;
        return m;
    }
}

struct SoundFontPlugin::Font
{
    juce::File file;
    juce::Time stamp;
    tsf* master = nullptr;
    std::vector<Preset> presets;

    ~Font()
    {
        const std::lock_guard<std::recursive_mutex> lock (fontMutex());
        tsf_close (master);
    }

    // The file's font, read now or shared from whoever read it already. A
    // file rewritten since it was read is read again; the old copy lives on
    // for as long as a plugin still plays it.
    static std::shared_ptr<Font> load (const juce::File& file)
    {
        // The mutex before the cache, so it is destroyed after it at exit.
        const std::lock_guard<std::recursive_mutex> lock (fontMutex());
        static std::map<juce::String, std::weak_ptr<Font>> cache;

        const auto key = file.getFullPathName();
        const auto stamp = file.getLastModificationTime();

        for (auto it = cache.begin(); it != cache.end();)
            it = it->second.expired() ? cache.erase (it) : std::next (it);

        if (auto it = cache.find (key); it != cache.end())
            if (auto existing = it->second.lock(); existing != nullptr && existing->stamp == stamp)
                return existing;

        juce::FileInputStream in (file);

        if (! in.openedOk())
            return {};

        tsf_stream stream { &in, &StreamAdapter::read, &StreamAdapter::skip };
        auto* loaded = tsf_load (&stream);

        if (loaded == nullptr)
            return {};

        if (tsf_get_presetcount (loaded) <= 0)
        {
            tsf_close (loaded);
            return {};
        }

        auto font = std::make_shared<Font>();
        font->file = file;
        font->stamp = stamp;
        font->master = loaded;

        for (int i = 0; i < loaded->presetNum; ++i)
        {
            const auto& preset = loaded->presets[i];
            font->presets.push_back ({ juce::String (juce::CharPointer_UTF8 (preset.presetName)).trim(),
                                       (int) preset.bank, (int) preset.preset });
        }

        // Bank, then program: the order a GM font is laid out in, and the
        // order a General MIDI chart lists it.
        std::stable_sort (font->presets.begin(), font->presets.end(), [] (const Preset& a, const Preset& b)
        {
            return a.bank != b.bank ? a.bank < b.bank : a.program < b.program;
        });

        cache[key] = font;
        return font;
    }
};

SoundFontPlugin::SoundFontPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
}

SoundFontPlugin::~SoundFontPlugin()
{
    notifyListenersOfDeletion();

    tsf* old = nullptr;

    {
        const juce::ScopedLock sl (synthLock);
        std::swap (old, synth);
    }

    const std::lock_guard<std::recursive_mutex> lock (fontMutex());
    tsf_close (old);
}

const std::vector<SoundFontPlugin::Preset>& SoundFontPlugin::getPresets() const
{
    static const std::vector<Preset> none;
    return font != nullptr ? font->presets : none;
}

void SoundFontPlugin::applyOutputSettings (tsf& f) const
{
    tsf_set_output (&f, TSF_STEREO_UNWEAVED, juce::roundToInt (sampleRate), outputGainDb);
}

void SoundFontPlugin::selectPreset (tsf& f, int bank, int program)
{
    if (tsf_get_presetindex (&f, bank, program) < 0 && font != nullptr && ! font->presets.empty())
    {
        bank = font->presets.front().bank;
        program = font->presets.front().program;
    }

    tsf_channel_set_bank_preset (&f, channel, bank, program);
    playingBank = bank;
    playingProgram = program;
}

void SoundFontPlugin::setSource (const juce::File& file, int bank, int program)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const bool sameFile = synth != nullptr && file == loadedFile
                            && file.getLastModificationTime() == loadedStamp;

    if (sameFile)
    {
        if (bank != requestedBank || program != requestedProgram)
        {
            requestedBank = bank;
            requestedProgram = program;

            const juce::ScopedLock sl (synthLock);
            selectPreset (*synth, bank, program);
        }

        return;
    }

    requestedBank = bank;
    requestedProgram = program;

    std::shared_ptr<Font> newFont;
    tsf* newSynth = nullptr;

    if (file == juce::File())
        loadError = {};
    else if (! file.existsAsFile())
        loadError = "File not found: " + file.getFileName();
    else if ((newFont = Font::load (file)) == nullptr)
        loadError = "Not a SoundFont this can read: " + file.getFileName();
    else
        loadError = {};

    if (newFont != nullptr)
    {
        {
            const std::lock_guard<std::recursive_mutex> lock (fontMutex());
            newSynth = tsf_copy (newFont->master);
        }

        if (newSynth == nullptr)
        {
            newFont.reset();
            loadError = "Out of memory loading " + file.getFileName();
        }
        else
        {
            // Everything that allocates happens here, before the audio thread
            // can see it: the voices, and the channel (the first channel call
            // creates it).
            tsf_set_max_voices (newSynth, maxVoices);
            applyOutputSettings (*newSynth);
        }
    }

    // The preset fallback reads the font's list, so the font has to be in
    // place first -- the old synth goes on playing the old font's samples,
    // which it still holds a share of, until the swap.
    auto oldFont = std::exchange (font, newFont);

    if (newSynth != nullptr)
        selectPreset (*newSynth, bank, program);

    tsf* oldSynth = newSynth;

    {
        const juce::ScopedLock sl (synthLock);
        std::swap (oldSynth, synth);
    }

    {
        const std::lock_guard<std::recursive_mutex> lock (fontMutex());
        tsf_close (oldSynth);
    }

    oldFont.reset();

    loadedFile = newFont != nullptr ? file : juce::File();
    loadedStamp = newFont != nullptr ? file.getLastModificationTime() : juce::Time();
}

void SoundFontPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    // Plugin::sampleRate, which the base class has already set from info.
    // Room for a whole block; a longer one is rendered in pieces.
    scratchSamples = juce::jmax (info.blockSizeSamples, 512);
    scratch.allocate ((size_t) scratchSamples * 2, true);

    const juce::ScopedLock sl (synthLock);

    if (synth != nullptr)
    {
        applyOutputSettings (*synth);
        tsf_channel_sounds_off_all (synth, channel);
    }
}

void SoundFontPlugin::deinitialise()
{
}

void SoundFontPlugin::reset()
{
    const juce::ScopedLock sl (synthLock);

    if (synth != nullptr)
        tsf_channel_sounds_off_all (synth, channel);
}

void SoundFontPlugin::handleMidi (tsf& f, const juce::MidiMessage& m)
{
    if (m.isNoteOn())
        tsf_channel_note_on (&f, channel, m.getNoteNumber(), m.getFloatVelocity());
    else if (m.isNoteOff())
        tsf_channel_note_off (&f, channel, m.getNoteNumber());
    else if (m.isAllSoundOff())
        tsf_channel_sounds_off_all (&f, channel);
    else if (m.isAllNotesOff())
        tsf_channel_note_off_all (&f, channel);
    else if (m.isPitchWheel())
        tsf_channel_set_pitchwheel (&f, channel, m.getPitchWheelValue());
    else if (m.isController())
    {
        // Bank select is left out: which preset plays is the model's to say,
        // and a stray CC 0 from a keyboard would otherwise switch it under
        // the user without the generator window knowing. Program changes are
        // ignored for the same reason.
        const int cc = m.getControllerNumber();

        if (cc != 0 && cc != 32)
            tsf_channel_midi_control (&f, channel, cc, m.getControllerValue());
    }
}

void SoundFontPlugin::renderSegment (tsf& f, juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    const int numChannels = buffer.getNumChannels();

    while (numSamples > 0)
    {
        const int chunk = juce::jmin (numSamples, scratchSamples);

        // Left in the first `chunk` floats, right in the next `chunk`.
        tsf_render_float (&f, scratch.get(), chunk, 0);

        buffer.copyFrom (0, startSample, scratch.get(), chunk);

        if (numChannels > 1)
            buffer.copyFrom (1, startSample, scratch.get() + chunk, chunk);
        else
            buffer.addFrom (0, startSample, scratch.get() + chunk, chunk);

        startSample += chunk;
        numSamples -= chunk;
    }
}

void SoundFontPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    auto& buffer = *fc.destBuffer;

    if (buffer.getNumChannels() == 0)
        return;

    buffer.clear (fc.bufferStartSample, fc.bufferNumSamples);

    // A font being swapped in is a block of silence, never a wait.
    const juce::ScopedTryLock sl (synthLock);

    if (! sl.isLocked() || synth == nullptr || scratchSamples == 0)
        return;

    auto& f = *synth;

    if (fc.bufferForMidiMessages != nullptr && fc.bufferForMidiMessages->isAllNotesOff)
        tsf_channel_sounds_off_all (&f, channel);

    // Up to each event, then the event, so a note lands on its own sample
    // rather than at the head of the block. Timestamps are seconds from the
    // start of the block.
    int pos = 0;

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (const auto& m : *fc.bufferForMidiMessages)
        {
            const int at = juce::jlimit (0, fc.bufferNumSamples,
                                         juce::roundToInt (m.getTimeStamp() * sampleRate));

            if (at > pos)
            {
                renderSegment (f, buffer, fc.bufferStartSample + pos, at - pos);
                pos = at;
            }

            handleMidi (f, m);
        }
    }

    if (pos < fc.bufferNumSamples)
        renderSegment (f, buffer, fc.bufferStartSample + pos, fc.bufferNumSamples - pos);

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);
}

} // namespace carve::plugins
