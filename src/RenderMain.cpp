#include "model/DemoSong.h"
#include "sync/EditSync.h"

namespace
{

void printUsage()
{
    std::cout << "orionish-te-render — Orionish (tracktion_engine) headless renderer\n"
                 "\n"
                 "Usage:\n"
                 "  orionish-te-render --demo <out.wav>          render the built-in demo song\n"
                 "  orionish-te-render --write-demo <out.orion>  write the demo song as a project file\n"
                 "  orionish-te-render <song.orion> <out.wav>    render an Orionish project file\n"
                 "  orionish-te-render <in.tracktionedit> <out.wav>  render a raw tracktion edit\n";
}

juce::File resolveFile (const juce::String& path)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile (path);
}

int renderEditToWav (te::Edit& edit, const juce::File& outputFile, const juce::String& name)
{
    // Render the whole edit plus a second of tail, all tracks, no UI thread.
    const auto endTime = te::TimePosition::fromSeconds (edit.getLength().inSeconds() + 1.0);

    juce::BigInteger tracksToDo;
    for (int i = 0; i < te::getAllTracks (edit).size(); ++i)
        tracksToDo.setBit (i);

    outputFile.deleteFile();
    if (! te::Renderer::renderToFile ("Render", outputFile, edit,
                                      { te::TimePosition(), endTime },
                                      tracksToDo, true, true, {}, false))
    {
        std::cerr << "Render failed\n";
        return 1;
    }

    std::cout << "Rendered \"" << name << "\" -> " << outputFile.getFullPathName() << "\n";
    return 0;
}

int renderSongToWav (te::Engine& engine, const orionish::model::Song& song, const juce::File& outputFile)
{
    auto edit = te::Edit::createSingleTrackEdit (engine);
    orionish::sync::syncSongToEdit (song, *edit);
    return renderEditToWav (*edit, outputFile, song.getName());
}

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    juce::StringArray args (argv + 1, argc - 1);

    if (args.size() != 2)
    {
        printUsage();
        return args.isEmpty() ? 0 : 1;
    }

    te::Engine engine { "orionish-te" };

    if (args[0] == "--demo")
        return renderSongToWav (engine, orionish::model::buildDemoSong(), resolveFile (args[1]));

    if (args[0] == "--write-demo")
    {
        auto file = resolveFile (args[1]);
        if (! orionish::model::buildDemoSong().saveToFile (file))
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

    auto song = orionish::model::Song::loadFromFile (inputFile);
    if (! song)
    {
        std::cerr << "Could not parse project file: " << args[0] << "\n";
        return 1;
    }
    return renderSongToWav (engine, *song, resolveFile (args[1]));
}
