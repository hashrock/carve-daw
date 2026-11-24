#include "PresetAudition.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "plugins/FactoryPresets.h"
#include "sync/EngineIds.h"

namespace carve::test
{

namespace
{
    enum class Phrase { drums, kick, bass, keys, lead, voice };

    struct Instrument
    {
        const char* label;
        const char* generatorType;
        const char* pluginType;
        Phrase phrase;
    };

    const Instrument instruments[]
    {
        { "Slate",          model::Generator::synthType,         te::FourOscPlugin::xmlTypeName,             Phrase::keys },
        { "Drum Synth",     model::Generator::drumSynthType,     plugins::DrumSynthPlugin::xmlTypeName,      Phrase::drums },
        { "Kick Designer",  model::Generator::gabberKickType,    plugins::GabberKickPlugin::xmlTypeName,     Phrase::kick },
        { "Acoustic Drums", model::Generator::acousticDrumsType, plugins::AcousticDrumsPlugin::xmlTypeName,  Phrase::drums },
        { "Modelled Drums", model::Generator::drumModelType,     plugins::DrumModelPlugin::xmlTypeName,      Phrase::drums },
        { "Electric Bass",  model::Generator::electricBassType,  plugins::ElectricBassPlugin::xmlTypeName,   Phrase::bass },
        { "Modelled Bass",  model::Generator::bassModelType,     plugins::BassModelPlugin::xmlTypeName,      Phrase::bass },
        { "Piano",          model::Generator::pianoType,         plugins::PianoPlugin::xmlTypeName,          Phrase::keys },
        { "Modelled Voice", model::Generator::vocalModelType,    plugins::VocalModelPlugin::xmlTypeName,     Phrase::voice },
    };

    // Slate is one instrument with every role; its presets say which by name.
    Phrase phraseFor (const Instrument& instrument, const juce::String& presetName)
    {
        if (instrument.phrase != Phrase::keys || juce::String (instrument.label) != "Slate")
            return instrument.phrase;

        if (presetName.containsIgnoreCase ("Bass"))
            return Phrase::bass;

        if (presetName.containsIgnoreCase ("Lead"))
            return Phrase::lead;

        return Phrase::keys;
    }

    // Two bars at 110 bpm, and a downbeat after them so a tail is heard.
    void writePhrase (model::Generator& generator, model::Playlist playlist, Phrase phrase)
    {
        auto pattern = generator.addPattern ("Audition", 9.0, nullptr);

        auto note = [&pattern] (double start, double length, int pitch, int velocity)
        {
            return pattern.addNote (start, length, pitch, velocity, nullptr);
        };

        switch (phrase)
        {
            case Phrase::drums:
                for (int bar = 0; bar < 2; ++bar)
                {
                    const double b = bar * 4.0;

                    for (double k : { 0.0, 1.5, 2.0 })
                        note (b + k, 0.25, 36, k == 0.0 ? 120 : 100);

                    note (b + 1.0, 0.25, 38, 112);
                    note (b + 3.0, 0.25, 38, bar == 0 ? 116 : 90);

                    for (int e = 0; e < 8; ++e)
                        if (! (bar == 0 && e == 7))
                            note (b + e * 0.5, 0.25, 42, e % 2 == 0 ? 96 : 70);
                }

                note (3.5, 0.5, 46, 100);
                // A fill into the downbeat.
                note (7.25, 0.25, 50, 100);
                note (7.5, 0.25, 48, 105);
                note (7.75, 0.25, 45, 110);
                note (8.0, 0.25, 36, 124);
                note (8.0, 0.25, 49, 115);
                break;

            case Phrase::kick:
                for (int beat = 0; beat <= 8; ++beat)
                    note (beat, 0.5, 36, 120);
                break;

            case Phrase::bass:
            {
                const struct { double start, length; int pitch, velocity; } line[] = {
                    { 0.0, 0.9, 33, 115 }, { 1.0, 0.4, 33, 90 }, { 1.5, 0.4, 40, 100 }, { 2.0, 0.9, 43, 105 },
                    { 3.0, 0.45, 45, 100 }, { 3.5, 0.45, 43, 95 },
                    { 4.0, 0.9, 38, 115 }, { 5.0, 0.4, 38, 90 }, { 5.5, 0.4, 45, 100 }, { 6.0, 0.9, 41, 105 },
                    { 7.0, 0.9, 40, 110 }, { 8.0, 0.9, 33, 120 },
                };

                for (const auto& n : line)
                    note (n.start, n.length, n.pitch, n.velocity);
                break;
            }

            case Phrase::keys:
                for (int pitch : { 45, 57, 60, 64 })
                    note (0.0, 3.8, pitch, 88);
                for (int pitch : { 41, 57, 60, 65 })
                    note (4.0, 3.8, pitch, 88);
                for (int pitch : { 45, 57, 60, 64, 69 })
                    note (8.0, 1.0, pitch, 96);
                for (const auto [start, pitch] : { std::pair { 0.0, 76 }, { 1.0, 74 }, { 2.0, 72 }, { 3.0, 71 },
                                                   { 4.0, 72 }, { 5.0, 69 }, { 6.0, 72 }, { 7.0, 77 } })
                    note (start, 0.9, pitch, 100);
                break;

            case Phrase::lead:
            {
                const struct { double start, length; int pitch; } melody[] = {
                    { 0.0, 0.45, 69 }, { 0.5, 0.45, 72 }, { 1.0, 0.9,  76 },
                    { 2.0, 0.45, 74 }, { 2.5, 0.45, 72 }, { 3.0, 0.9,  69 },
                    { 4.0, 0.45, 65 }, { 4.5, 0.45, 69 }, { 5.0, 0.9,  72 },
                    { 6.0, 2.8,  71 },
                };

                for (const auto& n : melody)
                    note (n.start, n.length, n.pitch, 104);
                break;
            }

            case Phrase::voice:
            {
                // さくら さくら やよいの そらは
                const struct { double start, length; int pitch; const char* lyric; } song[] = {
                    { 0.0, 0.9, 69, "\xe3\x81\x95" }, { 1.0, 0.9, 69, "\xe3\x81\x8f" }, { 2.0, 1.9, 71, "\xe3\x82\x89" },
                    { 4.0, 0.9, 69, "\xe3\x81\x95" }, { 5.0, 0.9, 69, "\xe3\x81\x8f" }, { 6.0, 1.9, 71, "\xe3\x82\x89" },
                    { 8.0, 0.9, 69, "\xe3\x82\x84" },
                };

                for (const auto& n : song)
                    note (n.start, n.length, n.pitch, 100).setLyric (juce::CharPointer_UTF8 (n.lyric), nullptr);
                break;
            }
        }

        playlist.addClip (generator, pattern, 0.0, nullptr);
    }

    struct Measured
    {
        double seconds = 0.0, peakDb = -200.0, rmsDb = -200.0, lowDb = -200.0;
        bool valid = false;
    };

    double toDb (double gain)  { return gain > 0.0 ? 20.0 * std::log10 (gain) : -200.0; }

    Measured measure (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        Measured m;

        if (reader == nullptr)
            return m;

        const int length = (int) reader->lengthInSamples;
        const int channels = (int) reader->numChannels;
        juce::AudioBuffer<float> buffer (channels, length);
        reader->read (&buffer, 0, length, 0, true, true);

        const double fs = reader->sampleRate;
        m.seconds = length / fs;
        m.valid = true;

        // Mono sum, and the same through a 2nd-order low pass at 200 Hz.
        std::vector<double> mono ((size_t) length), low ((size_t) length);
        double peak = 0.0;

        for (int i = 0; i < length; ++i)
        {
            double sum = 0.0;

            for (int c = 0; c < channels; ++c)
            {
                const auto x = (double) buffer.getSample (c, i);
                peak = std::max (peak, std::abs (x));
                sum += x;
            }

            mono[(size_t) i] = sum / channels;
        }

        {
            const auto coeffs = juce::IIRCoefficients::makeLowPass (fs, 200.0);
            juce::IIRFilter filter;
            filter.setCoefficients (coeffs);

            for (int i = 0; i < length; ++i)
                low[(size_t) i] = filter.processSingleSampleRaw ((float) mono[(size_t) i]);
        }

        // The loudest 400 ms, in 100 ms steps.
        const int window = (int) (0.4 * fs), hop = (int) (0.1 * fs);
        double bestRms = 0.0, lowAtBest = 0.0;

        for (int start = 0; start + window <= length; start += hop)
        {
            double all = 0.0, bass = 0.0;

            for (int i = start; i < start + window; ++i)
            {
                all += mono[(size_t) i] * mono[(size_t) i];
                bass += low[(size_t) i] * low[(size_t) i];
            }

            if (all > bestRms)
            {
                bestRms = all;
                lowAtBest = bass;
            }
        }

        m.peakDb = toDb (peak);
        m.rmsDb = toDb (std::sqrt (bestRms / std::max (1, window)));
        m.lowDb = bestRms > 0.0 ? 10.0 * std::log10 (std::max (1.0e-20, lowAtBest / bestRms)) : -200.0;
        return m;
    }

    // The plugin's tree with a preset applied, as a song would store it.
    juce::ValueTree capturePreset (te::Edit& edit, const char* pluginType, int index)
    {
        auto plugin = edit.getPluginCache().createNewPlugin (pluginType, {});

        if (plugin == nullptr || ! plugins::presets::applyFactoryPreset (*plugin, index))
            return {};

        auto copy = plugin->state.createCopy();
        copy.removeProperty (te::IDs::id, nullptr);
        copy.removeProperty (sync::effectIdProperty, nullptr);
        return copy;
    }

    juce::String fixed (double v, int decimals)  { return v <= -199.0 ? juce::String ("-inf") : juce::String (v, decimals); }
} // namespace

int runPresetAudition (tracktion::Engine& engine, RenderSong render, const juce::File& folder, const juce::String& filter)
{
    folder.createDirectory();

    auto scratch = te::Edit::createSingleTrackEdit (engine);
    int problems = 0;

    juce::String markdown;
    markdown << "| Instrument | # | Preset | Peak dBFS | RMS dBFS (400 ms max) | Low <200 Hz (dB of total) | CPU (% of real time) | File |\n"
             << "|---|---|---|---|---|---|---|---|\n";

    std::printf ("%-15s %3s  %-32s %7s %7s %7s %7s  %s\n", "instrument", "#", "preset", "peak", "rms", "low", "cpu%", "note");

    for (const auto& instrument : instruments)
    {
        auto plugin = scratch->getPluginCache().createNewPlugin (instrument.pluginType, {});

        if (plugin == nullptr)
            continue;

        const auto names = plugins::presets::getFactoryPresetNames (*plugin);

        for (int i = 0; i < names.size(); ++i)
        {
            if (filter.isNotEmpty() && ! juce::String (instrument.label).containsIgnoreCase (filter)
                 && ! names[i].containsIgnoreCase (filter))
                continue;

            juce::StringArray notes;

            // The preset bar's tick: applied, it should read as itself.
            plugins::presets::applyFactoryPreset (*plugin, i);

            if (plugins::presets::findMatchingFactoryPreset (*plugin) != i)
                notes.add ("not recognised after loading");

            const auto stored = capturePreset (*scratch, instrument.pluginType, i);

            auto song = model::Song::create (juce::String (instrument.label) + " - " + names[i]);
            song.setTempo (110.0, nullptr);
            auto generator = song.addGenerator (instrument.label, instrument.generatorType, nullptr);
            generator.setInternalState (stored, nullptr);
            writePhrase (generator, song.getPlaylist(), phraseFor (instrument, names[i]));

            const auto file = folder.getChildFile (juce::File::createLegalFileName (juce::String (instrument.label) + " "
                                                                                     + juce::String (i + 1).paddedLeft ('0', 2)
                                                                                     + " " + names[i]) + ".wav");

            const auto started = std::chrono::steady_clock::now();
            const bool rendered = render (song, file, 44100.0);
            const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();

            const auto m = rendered ? measure (file) : Measured {};
            const auto cpu = m.seconds > 0.0 ? 100.0 * elapsed / m.seconds : 0.0;

            if (! m.valid)
                notes.add ("render failed");
            else if (m.rmsDb < -40.0)
                notes.add ("near silent");
            else if (m.peakDb > -0.1)
                notes.add ("clips");

            if (notes.size() > 0)
                ++problems;

            std::printf ("%-15s %3d  %-32s %7s %7s %7s %7.1f  %s\n", instrument.label, i + 1,
                         names[i].substring (0, 32).toRawUTF8(), fixed (m.peakDb, 1).toRawUTF8(),
                         fixed (m.rmsDb, 1).toRawUTF8(), fixed (m.lowDb, 1).toRawUTF8(), cpu,
                         notes.joinIntoString ("; ").toRawUTF8());
            std::fflush (stdout);

            markdown << "| " << instrument.label << " | " << (i + 1) << " | " << names[i]
                     << " | " << fixed (m.peakDb, 1) << " | " << fixed (m.rmsDb, 1) << " | " << fixed (m.lowDb, 1)
                     << " | " << juce::String (cpu, 1) << " | " << file.getFileName()
                     << (notes.isEmpty() ? juce::String() : " (" + notes.joinIntoString ("; ") + ")") << " |\n";
        }
    }

    folder.getChildFile ("presets.md").replaceWithText (markdown);
    std::printf ("\n%d problem(s). Table: %s\n", problems, folder.getChildFile ("presets.md").getFullPathName().toRawUTF8());
    return problems;
}

}
