#include "EngineSetup.h"
#include "model/DemoSong.h"
#include "sync/EditSync.h"

namespace
{

void printUsage()
{
    std::cout << "carve-render - Carve (tracktion_engine) headless renderer\n"
                 "\n"
                 "Usage:\n"
                 "  carve-render --demo <out.wav>          render the built-in demo song\n"
                 "  carve-render --write-demo <out.carve>  write the demo song as a project file\n"
                 "  carve-render <song.carve> <out.wav>    render an Carve project file\n"
                 "  carve-render <in.tracktionedit> <out.wav>  render a raw tracktion edit\n"
                 "  carve-render --scan                    scan VST3/AU plugins (cached in settings)\n"
                 "  carve-render --plugin-demo <name> <out.wav>\n"
                 "        render the demo song using the named (substring-matched) instrument plugin\n";
}

juce::File resolveFile (const juce::String& path)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile (path);
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
    auto& deviceManager = engine.getDeviceManager();

    const te::Edit::ScopedRenderStatus renderStatus (edit, true);

    te::Renderer::Parameters params (edit);
    params.destFile = outputFile;
    params.audioFormat = engine.getAudioFileFormatManager().getDefaultFormat();
    params.bitDepth = 24;
    params.sampleRateForAudio = deviceManager.getSampleRate();
    params.blockSizeForAudio = deviceManager.getBlockSize();
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

    std::cout << "Rendered \"" << name << "\" -> " << outputFile.getFullPathName() << "\n";
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

    if (args.isEmpty() || (args[0] == "--scan" ? args.size() != 1
                           : args[0] == "--plugin-demo" ? args.size() != 3
                                                        : args.size() != 2))
    {
        printUsage();
        return args.isEmpty() ? 0 : 1;
    }

    auto enginePtr = carve::createEngine (std::make_unique<carve::HeadlessUIBehaviour>(),
                                          false, /*singleThreadedAudio*/ true);
    auto& engine = *enginePtr;

    if (args[0] == "--scan")
        return scanPlugins (engine);

    if (args[0] == "--plugin-demo")
        return renderPluginDemo (engine, args[1], resolveFile (args[2]));

    if (args[0] == "--demo")
        return renderSongToWav (engine, carve::model::buildDemoSong(), resolveFile (args[1]));

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
