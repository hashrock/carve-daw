#include <cstring>
#include <optional>

#include "EngineSetup.h"
#include "model/DemoSong.h"
#include "model/SampleSongs.h"
#include "sync/EditSync.h"
#include "NoteOffPlayback.h"

namespace
{

// What a render is at, unless --rate says otherwise.
//
// A constant rather than the audio device's, which is what this used to use.
// The device rate depends on whether the GUI happens to be holding the
// hardware, so the same song rendered twice could come out at 44.1k once and
// 48k the next time -- and a bit-identical render (see the README) is no use
// as a regression check if the two files are not even the same length.
constexpr double defaultSampleRate = 44100.0;

// Below this the render is not audio, and above it the engine is being asked
// for something no format here writes.
constexpr double minSampleRate = 8000.0;
constexpr double maxSampleRate = 384000.0;

// Fixed for the same reason as the sample rate, and there is no option for it
// because nothing about the result should depend on it. It came from the
// audio device too -- and the device setup is stored under the same
// application name the GUI uses, so changing the buffer size in the GUI moved
// the block boundaries the CLI renders on. Anything evaluated per block
// (automation ramps, modulators, a plugin's own smoothing) lands differently
// either side of that, which is exactly the low-bit drift the single-threaded
// render exists to rule out.
constexpr int renderBlockSize = 512;

void printUsage()
{
    std::cout << "carve-render - Carve (tracktion_engine) headless renderer\n"
                 "\n"
                 "Usage:\n"
                 "  carve-render --demo <out.wav>          render the built-in demo song\n"
                 "  carve-render --sample <name> <out.wav> render one of the sample songs\n"
                 "        (a substring of its name, or its number; --sample with no name lists them)\n"
                 "  carve-render --write-demo <out.carve>  write the demo song as a project file\n"
                 "  carve-render <song.carve> <out.wav>    render an Carve project file\n"
                 "  carve-render <in.tracktionedit> <out.wav>  render a raw tracktion edit\n"
                 "  carve-render --scan                    scan VST3/AU plugins (cached in settings)\n"
                 "  carve-render --plugin-demo <name> <out.wav>\n"
                 "        render the demo song using the named (substring-matched) instrument plugin\n"
                 "  carve-render --check-note-offs         play a song and delete what is sounding;\n"
                 "        exits with the number of notes left ringing (see tests/NoteOffPlayback.cpp)\n"
                 "\n"
                 "Options:\n"
                 "  --rate <hz>    sample rate of the rendered file (default 44100)\n";
}

// The rate every render in this process runs at. Set once from the command
// line before anything renders; a global because it is a property of the run
// rather than of any one of the render entry points, all of which would
// otherwise have to thread it through unchanged.
double renderSampleRate = defaultSampleRate;

// Pulls "--rate <hz>" out of the argument list, leaving the positional
// arguments behind for the command dispatch to read as it always has.
// Returns false with a message written for a missing or nonsensical value,
// rather than quietly rendering at the default.
bool takeSampleRateOption (juce::StringArray& args)
{
    const auto index = args.indexOf ("--rate");
    if (index < 0)
        return true;

    if (index + 1 >= args.size())
    {
        std::cerr << "--rate needs a sample rate in Hz\n";
        return false;
    }

    const auto value = args[index + 1].getDoubleValue();
    if (value < minSampleRate || value > maxSampleRate)
    {
        std::cerr << "Sample rate out of range (" << (int) minSampleRate << ".."
                  << (int) maxSampleRate << " Hz): " << args[index + 1] << "\n";
        return false;
    }

    renderSampleRate = value;
    args.removeRange (index, 2);
    return true;
}

juce::File resolveFile (const juce::String& path)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile (path);
}

// Offset of a named chunk's payload in a RIFF/WAVE file, or nothing if the
// file has no such chunk. Walked properly rather than searched for: "bext"
// can occur inside audio data, and tracktion's own scan (applyBWAVStartTime)
// takes the *last* match in the first 2KB for exactly that reason.
std::optional<juce::int64> findChunkData (juce::FileInputStream& in, const char* fourCC)
{
    char id[4];

    if (in.read (id, 4) != 4 || memcmp (id, "RIFF", 4) != 0)
        return {};

    in.readInt();   // total size, which we do not need

    if (in.read (id, 4) != 4 || memcmp (id, "WAVE", 4) != 0)
        return {};

    while (! in.isExhausted())
    {
        if (in.read (id, 4) != 4)
            break;

        const auto size = (juce::int64) (juce::uint32) in.readInt();
        const auto dataStart = in.getPosition();

        if (memcmp (id, fourCC, 4) == 0)
            return dataStart;

        // Chunks are word-aligned, so an odd size is followed by a pad byte.
        in.setPosition (dataStart + size + (size & 1));

        if (in.getPosition() <= dataStart)   // a zero or bogus size would loop forever
            break;
    }

    return {};
}

// Replaces the BWF origination date and time with a fixed one.
//
// The last thing standing between a render and a usable md5. The audio is
// already bit-identical, but JUCE stamps the "bext" chunk with the wall
// clock, so two renders of the same song a second apart differ in the bytes
// holding the time. Nothing the caller passes can prevent it: tracktion adds
// the stamp from inside the render (NodeRenderContext), and it does so with
// StringPairArray::addArray, which overwrites whatever Parameters::metadata
// held.
//
// Overwriting the field rather than dropping the chunk: the same chunk also
// carries the render's start time, which is real information a DAW importing
// the file will use, and removing a chunk means rewriting the file rather
// than eighteen bytes of it.
void stampFixedOriginationTime (const juce::File& file)
{
    // The bext payload, per the BWF spec: 256 bytes of description, 32 of
    // originator, 32 of originator reference, then the date and the time as
    // fixed-width ASCII with no terminator.
    constexpr juce::int64 dateOffset = 256 + 32 + 32;
    const juce::String fixedDateAndTime ("1970-01-01" "00:00:00");   // 10 + 8 bytes

    juce::int64 position = 0;

    {
        juce::FileInputStream in (file);

        if (! in.openedOk())
            return;

        const auto chunk = findChunkData (in, "bext");

        if (! chunk)
            return;   // not a BWF file, so nothing dated to fix

        position = *chunk + dateOffset;
    }

    juce::FileOutputStream out (file);

    if (out.openedOk() && out.setPosition (position))
        out.write (fixedDateAndTime.toRawUTF8(), (size_t) fixedDateAndTime.length());
}

int renderEditToWav (te::Edit& edit, const juce::File& outputFile, const juce::String& name)
{
    // Render the whole edit plus a second of tail.
    //
    // This goes through Renderer::Parameters rather than the convenient
    // renderToFile(task, file, edit, range, tracks, ...) overload on purpose:
    // that one wraps the render in a FreezePointPlugin::ScopedTrackSoloIsolator,
    // which unmutes every track it is asked to render ("useful for rendering",
    // per tracktion). A generator muted in the mixer would still be audible in
    // the exported file.
    auto& engine = edit.engine;

    const te::Edit::ScopedRenderStatus renderStatus (edit, true);

    te::Renderer::Parameters params (edit);
    params.destFile = outputFile;
    params.audioFormat = engine.getAudioFileFormatManager().getDefaultFormat();
    params.bitDepth = 24;
    params.sampleRateForAudio = renderSampleRate;
    params.blockSizeForAudio = renderBlockSize;
    params.time = { te::TimePosition(),
                    te::TimePosition::fromSeconds (edit.getLength().inSeconds() + 1.0) };
    params.usePlugins = true;
    params.useMasterPlugins = true;
    params.tracksToDo = te::toBitSet (te::getAllTracks (edit));

    outputFile.deleteFile();
    if (te::Renderer::renderToFile ("Render", params) == juce::File())
    {
        std::cerr << "Render failed\n";
        return 1;
    }

    stampFixedOriginationTime (outputFile);

    std::cout << "Rendered \"" << name << "\" at " << (int) renderSampleRate << " Hz -> "
              << outputFile.getFullPathName() << "\n";
    return 0;
}

int renderSongToWav (te::Engine& engine, const carve::model::Song& song, const juce::File& outputFile)
{
    auto edit = te::Edit::createSingleTrackEdit (engine);
    carve::sync::syncSongToEdit (song, *edit);

    // No message loop is running here, so nothing would otherwise deliver the
    // callback a sampler waits on and it would render silence.
    carve::sync::flushSamplerLoads (*edit);

    return renderEditToWav (*edit, outputFile, song.getName());
}

// By number as listed, or by any substring of the name -- the same courtesy
// --plugin-demo does, and for the same reason: nobody wants to type "Neon
// Streets" exactly to hear whether it still renders.
int renderSampleSong (te::Engine& engine, const juce::String& nameOrNumber,
                      const juce::File& outputFile)
{
    const auto names = carve::model::sampleSongNames();

    int index = -1;

    if (const auto number = nameOrNumber.getIntValue();
        number >= 1 && number <= names.size() && nameOrNumber.containsOnly ("0123456789"))
        index = number - 1;
    else
        for (int i = 0; i < names.size(); ++i)
            if (index < 0 && names[i].containsIgnoreCase (nameOrNumber))
                index = i;

    if (index < 0)
    {
        std::cerr << "No sample song matching \"" << nameOrNumber << "\" - run --sample to list them\n";
        return 1;
    }

    return renderSongToWav (engine, carve::model::buildSampleSong (index), outputFile);
}

int scanPlugins (te::Engine& engine)
{
    auto& pluginManager = engine.getPluginManager();

    for (auto* format : pluginManager.pluginFormatManager.getFormats())
    {
        std::cout << "Scanning " << format->getName() << "...\n";
        juce::PluginDirectoryScanner scanner (
            pluginManager.knownPluginList, *format,
            format->getDefaultLocationsToSearch(), true,
            engine.getTemporaryFileManager().getTempFile ("scan-dead-mans-pedal"));

        juce::String pluginBeingScanned;
        while (scanner.scanNextFile (true, pluginBeingScanned)) {}
    }

    // PluginManager normally saves the list from an async change callback,
    // which never runs in this CLI - persist it explicitly.
    if (auto xml = pluginManager.knownPluginList.createXml())
        engine.getPropertyStorage().setXmlProperty (te::SettingID::knownPluginList64, *xml);

    const auto types = pluginManager.knownPluginList.getTypes();
    std::cout << "\nFound " << types.size() << " plugins:\n";
    for (const auto& type : types)
        std::cout << (type.isInstrument ? "  [inst] " : "  [fx]   ")
                  << type.pluginFormatName << "  " << type.name
                  << "  (" << type.fileOrIdentifier << ")\n";
    return 0;
}

int renderPluginDemo (te::Engine& engine, const juce::String& nameSubstring,
                      const juce::File& outputFile)
{
    const auto types = engine.getPluginManager().knownPluginList.getTypes();
    if (types.isEmpty())
    {
        std::cerr << "No plugins known - run --scan first\n";
        return 1;
    }

    const juce::PluginDescription* match = nullptr;
    for (const auto& type : types)
        if (type.isInstrument && type.name.containsIgnoreCase (nameSubstring))
        {
            match = &type;
            break;
        }

    if (match == nullptr)
    {
        std::cerr << "No instrument plugin matching \"" << nameSubstring << "\"\n";
        return 1;
    }

    std::cout << "Using " << match->pluginFormatName << ": " << match->name << "\n";

    auto song = carve::model::buildDemoSong();
    for (auto generator : song.getGenerators())
    {
        generator.state.setProperty (carve::model::ids::type, "plugin", nullptr);
        generator.setPlugin (*match, nullptr);
    }
    return renderSongToWav (engine, song, outputFile);
}

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    juce::StringArray args (argv + 1, argc - 1);

    if (! takeSampleRateOption (args))
        return 1;

    const int expectedArgs = args[0] == "--scan" || args[0] == "--check-note-offs" ? 1
                           : args[0] == "--plugin-demo" || args[0] == "--sample"     ? 3
                                                                                    : 2;

    // --sample on its own lists what there is to ask for, rather than being
    // an argument-count error about a name the user does not have yet.
    if (args.size() == 1 && args[0] == "--sample")
    {
        const auto names = carve::model::sampleSongNames();

        std::cout << "Sample songs:\n";

        for (int i = 0; i < names.size(); ++i)
            std::cout << "  " << (i + 1) << "  " << names[i] << "\n";

        return 0;
    }
    if (args.isEmpty() || args.size() != expectedArgs)
    {
        printUsage();
        return args.isEmpty() ? 0 : 1;
    }

    auto enginePtr = carve::createEngine (std::make_unique<carve::HeadlessUIBehaviour>(),
                                          false, /*singleThreadedAudio*/ true);
    auto& engine = *enginePtr;

    if (args[0] == "--scan")
        return scanPlugins (engine);

    if (args[0] == "--check-note-offs")
        return carve::test::runNoteOffPlaybackChecks (engine);

    if (args[0] == "--plugin-demo")
        return renderPluginDemo (engine, args[1], resolveFile (args[2]));

    if (args[0] == "--demo")
        return renderSongToWav (engine, carve::model::buildDemoSong(), resolveFile (args[1]));

    if (args[0] == "--sample")
        return renderSampleSong (engine, args[1], resolveFile (args[2]));

    if (args[0] == "--write-demo")
    {
        auto file = resolveFile (args[1]);
        if (! carve::model::buildDemoSong().saveToFile (file))
        {
            std::cerr << "Failed to write " << file.getFullPathName() << "\n";
            return 1;
        }
        std::cout << "Wrote demo project -> " << file.getFullPathName() << "\n";
        return 0;
    }

    auto inputFile = resolveFile (args[0]);
    if (! inputFile.existsAsFile())
    {
        std::cerr << "Could not find input file: " << args[0] << "\n";
        return 1;
    }

    if (inputFile.hasFileExtension ("tracktionedit"))
    {
        auto edit = te::loadEditFromFile (engine, inputFile);
        if (edit == nullptr)
        {
            std::cerr << "Could not parse edit file: " << args[0] << "\n";
            return 1;
        }
        return renderEditToWav (*edit, resolveFile (args[1]), inputFile.getFileName());
    }

    auto song = carve::model::Song::loadFromFile (inputFile);
    if (! song)
    {
        std::cerr << "Could not parse project file: " << args[0] << "\n";
        return 1;
    }
    return renderSongToWav (engine, *song, resolveFile (args[1]));
}
